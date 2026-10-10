// Custom attributes: each usage becomes a C factory that constructs the
// attribute object (ctor arguments, then named fields and properties). The
// runtime builds them on demand for reflection and for Unity's mono API.
// Attributes nothing at run time looks at (compiler, debugger, interop,
// security markers) are skipped to keep the tables small.
using System.Text;
using Mono.Cecil;

sealed record AttrUse(TT Type, MI Ctor, CustomAttribute Data);

sealed class Attrs
{
    readonly Gen g; readonly World w;
    // (owner type, provider) -> uses
    public readonly Dictionary<(TT, ICustomAttributeProvider), List<AttrUse>> Uses = new();
    readonly HashSet<TT> scanned = new();

    public Attrs(Gen g) { this.g = g; w = g.W; }

    static bool Skip(TypeReference t)
    {
        var ns = t.Namespace;
        if (ns.StartsWith("System.Runtime.CompilerServices") || ns.StartsWith("System.Diagnostics") || ns.StartsWith("System.Security")
            || ns.StartsWith("System.Runtime.ConstrainedExecution") || ns.StartsWith("System.Runtime.Versioning")) return true;
        if (ns == "System.Runtime.InteropServices" && t.Name != "StructLayoutAttribute") return true;
        if (t.Name.StartsWith("Mono") && t.Name.EndsWith("Attribute")) return true;
        return t.Name is "CLSCompliantAttribute" or "ComVisibleAttribute" or "AssemblyTitleAttribute" or "TargetedPatchingOptOutAttribute";
    }

    // Called from the reachability loop: compiles the constructors and setters
    // of every attribute on the types seen so far. Returns whether it added work.
    public bool Scan(IEnumerable<TT> types)
    {
        bool added = false;
        foreach (var t in types.ToList())
        {
            if (t.K != TK.Def || t.IsOpen || !scanned.Add(t)) continue;
            added |= Collect(t, t.Def);
            foreach (var m in t.Def.Methods) added |= Collect(t, m);
            foreach (var f in t.Def.Fields) added |= Collect(t, f);
            foreach (var p in t.Def.Properties) added |= Collect(t, p);
        }
        return added;
    }

    bool Collect(TT owner, ICustomAttributeProvider p)
    {
        if (!p.HasCustomAttributes) return false;
        var list = new List<AttrUse>();
        foreach (var ca in p.CustomAttributes)
        {
            if (Skip(ca.AttributeType)) continue;
            try
            {
                var at = w.Conv(ca.AttributeType, Array.Empty<TT>(), Array.Empty<TT>());
                var ctor = w.ConvMethod(ca.Constructor, Array.Empty<TT>(), Array.Empty<TT>());
                if (ctor == null) continue;
                g.Construct(at);
                g.Need(ctor);
                foreach (var na in ca.Properties)
                {
                    var setter = FindSetter(at, na.Name);
                    if (setter != null) g.Need(setter);
                }
                foreach (var arg in ca.ConstructorArguments.Concat(ca.Fields.Select(f => f.Argument)).Concat(ca.Properties.Select(f => f.Argument)))
                    TouchArg(arg);
                list.Add(new AttrUse(at, ctor, ca));
            }
            catch (Exception e) when (e is EmitError or UnresolvedGeneric) { }
        }
        if (list.Count == 0) return false;
        Uses[(owner, p)] = list;
        return true;
    }

    void TouchArg(CustomAttributeArgument a)
    {
        if (a.Value is TypeReference tr) g.Klass(w.Conv(tr, Array.Empty<TT>(), Array.Empty<TT>()));
        if (a.Value is CustomAttributeArgument inner) { TouchArg(inner); g.Construct(w.Conv(inner.Type, Array.Empty<TT>(), Array.Empty<TT>())); }
        if (a.Value is CustomAttributeArgument[] arr)
        {
            g.Construct(w.Conv(a.Type, Array.Empty<TT>(), Array.Empty<TT>()));
            foreach (var x in arr) TouchArg(x);
        }
    }

    MI? FindSetter(TT at, string name)
    {
        for (var c = at; c != null; c = w.BaseOf(c))
        {
            var p = c.Def.Properties.FirstOrDefault(x => x.Name == name);
            if (p?.SetMethod != null) return w.Method(c, p.SetMethod, Array.Empty<TT>());
        }
        return null;
    }

    FI? FindField(TT at, string name)
    {
        for (var c = at; c != null; c = w.BaseOf(c))
        {
            var f = c.Def.Fields.FirstOrDefault(x => x.Name == name);
            if (f != null) return new FI { Owner = c, Def = f, Type = w.FieldType(c, f) };
        }
        return null;
    }

    // ---- emission ----
    int n;
    public readonly StringBuilder Factories = new();
    readonly Dictionary<(TT, ICustomAttributeProvider), string> tables = new();

    // Returns "table, count" for a provider, or "0, 0".
    public string Table(TT owner, ICustomAttributeProvider p, Func<TT, string> kref)
    {
        if (!Uses.TryGetValue((owner, p), out var list)) return "0, 0";
        if (tables.TryGetValue((owner, p), out var name)) return $"{name}, {list.Count}";
        var entries = new List<string>();
        foreach (var u in list)
        {
            if (!g.IsReached(u.Ctor)) continue;
            var fn = $"CA{n++}";
            var body = new StringBuilder();
            body.Append($"static void* {fn}(void) {{\n    void* o = nr_new(&K{u.Type.Id});\n");
            var args = new List<string> { "o" };
            for (int i = 0; i < u.Data.ConstructorArguments.Count; i++)
                args.Add(Value(u.Data.ConstructorArguments[i], w.ParamType(u.Ctor, i), body, kref));
            body.Append($"    {g.NameOf(u.Ctor)}({string.Join(", ", args)});\n");
            foreach (var na in u.Data.Fields)
            {
                var f = FindField(u.Type, na.Name);
                if (f == null) continue;
                var v = Value(na.Argument, f.Type, body, kref);
                var acc = $"((O{u.Type.Id}*)o)->{g.FieldC(f)}";
                body.Append($"    {acc} = {v};\n");
            }
            foreach (var na in u.Data.Properties)
            {
                var s = FindSetter(u.Type, na.Name);
                if (s == null || !g.IsReached(s)) continue;
                body.Append($"    {g.NameOf(s)}(o, {Value(na.Argument, w.ParamType(s, 0), body, kref)});\n");
            }
            body.Append("    return o;\n}\n");
            Factories.Append(body);
            entries.Add($"{{ &K{u.Type.Id}, {fn} }}");
        }
        if (entries.Count == 0) { tables[(owner, p)] = "0"; return "0, 0"; }
        name = $"AT{n++}";
        Factories.Append($"static const NrAttr {name}[] = {{ {string.Join(", ", entries)} }};\n");
        tables[(owner, p)] = name;
        return $"{name}, {entries.Count}";
    }

    int tmp;
    string Value(CustomAttributeArgument a, TT want, StringBuilder body, Func<TT, string> kref)
    {
        var v = a.Value;
        if (v is CustomAttributeArgument inner)
        {
            // object-typed parameter: box the inner value
            var it = w.Conv(inner.Type, Array.Empty<TT>(), Array.Empty<TT>());
            var raw = Value(inner, it, body, kref);
            if (!it.IsValueType) return raw;
            var t = $"t{tmp++}";
            body.Append($"    {g.Lay.C(it)} {t} = {raw};\n");
            return $"nr_box(&K{it.Id}, &{t})";
        }
        if (v is CustomAttributeArgument[] arr)
        {
            var at = w.Conv(a.Type, Array.Empty<TT>(), Array.Empty<TT>());
            var et = at.Elem;
            var t = $"arr{tmp++}";
            body.Append($"    void* {t} = nr_newarr(&K{at.Id}, {arr.Length});\n");
            for (int i = 0; i < arr.Length; i++)
            {
                var x = Value(arr[i], et, body, kref);
                body.Append(g.Lay.Kind(et) == SK.P
                    ? $"    ((void**)NR_ADATA({t}))[{i}] = {x};\n"
                    : $"    (({g.Lay.C(et)}*)NR_ADATA({t}))[{i}] = {x};\n");
            }
            return t;
        }
        if (v == null) return "0";
        if (v is TypeReference tr) { var k = kref(w.Conv(tr, Array.Empty<TT>(), Array.Empty<TT>())); return k == "0" ? "0" : $"nr_type_object({k})"; }
        if (v is string s) return g.Str(s);
        if (v is bool b) return b ? "1" : "0";
        if (v is float f) return float.IsFinite(f) ? f.ToString("R", System.Globalization.CultureInfo.InvariantCulture) + (f % 1 == 0 && !f.ToString("R").Contains('E') ? ".0f" : "f") : $"nr_f32(0x{BitConverter.SingleToUInt32Bits(f):x}u)";
        if (v is double d) return double.IsFinite(d) ? d.ToString("R", System.Globalization.CultureInfo.InvariantCulture) + (d % 1 == 0 && !d.ToString("R").Contains('E') ? ".0" : "") : $"nr_f64(0x{BitConverter.DoubleToUInt64Bits(d):x}ull)";
        if (v is char c) return ((int)c).ToString();
        if (v is long l) return $"(int64_t)0x{(ulong)l:x}ull";
        if (v is ulong ul) return $"(int64_t)0x{ul:x}ull";
        if (v is uint ui) return $"(int32_t)0x{ui:x}u";
        if (v is int iv) return iv == int.MinValue ? "(int32_t)0x80000000u" : iv.ToString();
        return Convert.ToInt64(v).ToString();
    }
}
