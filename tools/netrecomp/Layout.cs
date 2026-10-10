// C representation, field layout, vtables and interface maps for concrete types.
// Object layout follows mono's 32-bit embedding ABI, because Unity's native
// engine reads strings, arrays and object headers directly:
//   object  = { NrClass* klass; void* sync; fields... }
//   string  = { header; int32 length; uint16 chars[] }      chars at +12
//   array   = { header; bounds*; uint32 max_length; data }  data at +16
// See docs/runtime.md.
using Mono.Cecil;

enum SK { I4, I8, R4, R8, I, P, VT }   // eval-stack kinds; P covers objects and managed pointers

sealed class Layout
{
    public string CType = "";          // C type of a value of this type
    public SK Kind;
    public List<(FI f, string cname)> Inst = new();     // instance fields incl. base, in order
    public List<(FI f, string cname)> Statics = new();
    public Dictionary<FieldDefinition, string> FieldName = new();
    public List<MI?> Slots = new();                     // class vtable
    public Dictionary<MethodDefinition, int> SlotOf = new();
    public List<(TT iface, MI?[] impls)> Ifaces = new();
    public Dictionary<TT, int[]> IfaceSlots = new();    // iface -> class slot per method (classes)
    public List<MethodDefinition> IfaceMethods = new(); // for interfaces: their dispatch order
    public bool Done;
    public string? Error;
}

sealed class Layouts
{
    readonly World w;
    public Layouts(World w) { this.w = w; }

    static readonly Dictionary<string, (string c, SK k)> prim = new()
    {
        ["System.Boolean"] = ("uint8_t", SK.I4), ["System.Char"] = ("uint16_t", SK.I4),
        ["System.SByte"] = ("int8_t", SK.I4), ["System.Byte"] = ("uint8_t", SK.I4),
        ["System.Int16"] = ("int16_t", SK.I4), ["System.UInt16"] = ("uint16_t", SK.I4),
        ["System.Int32"] = ("int32_t", SK.I4), ["System.UInt32"] = ("uint32_t", SK.I4),
        ["System.Int64"] = ("int64_t", SK.I8), ["System.UInt64"] = ("uint64_t", SK.I8),
        ["System.Single"] = ("float", SK.R4), ["System.Double"] = ("double", SK.R8),
        ["System.IntPtr"] = ("intptr_t", SK.I), ["System.UIntPtr"] = ("uintptr_t", SK.I),
        ["System.Void"] = ("void", SK.I4),
    };

    public static bool IsPrim(TT t) => t.K == TK.Def && t.Def.Module.Assembly.Name.Name == "mscorlib" && prim.ContainsKey(t.Def.FullName);
    public static bool IsVoid(TT t) => t.K == TK.Def && t.Def.FullName == "System.Void" && t.Def.Module.Assembly.Name.Name == "mscorlib";

    public TT EnumBase(TT t) => w.FieldType(t, t.Def.Fields.First(f => !f.IsStatic));

    public Layout Of(TT t)
    {
        if (t.L != null) return t.L;
        var L = t.L = new Layout();
        switch (t.K)
        {
            case TK.Ptr: case TK.FnPtr: L.CType = "intptr_t"; L.Kind = SK.I; break;
            case TK.ByRef: L.CType = "void*"; L.Kind = SK.P; break;
            case TK.SzArray: case TK.Array: L.CType = "void*"; L.Kind = SK.P; break;
            default:
                if (IsPrim(t)) { (L.CType, L.Kind) = prim[t.Def.FullName]; }
                else if (t.IsEnum) { var b = Of(EnumBase(t)); L.CType = b.CType; L.Kind = b.Kind; }
                else if (t.IsValueType) { L.CType = "T" + Id(t); L.Kind = SK.VT; }
                else { L.CType = "void*"; L.Kind = SK.P; }
                break;
        }
        return L;
    }

    // Ids are handed out on first use so the generated tables only hold what's used.
    public readonly List<TT> Used = new();
    public int Id(TT t)
    {
        if (t.Id < 0) { t.Id = Used.Count; Used.Add(t); }
        return t.Id;
    }

    public string C(TT t) => Of(t).CType;
    public SK Kind(TT t) => Of(t).Kind;

    static string San(string s)
    {
        var b = new System.Text.StringBuilder();
        foreach (var ch in s) b.Append(char.IsAsciiLetterOrDigit(ch) ? ch : '_');
        return b.ToString();
    }

    // Fields, vtable and interface map. Separate from Of() because it recurses
    // into base types and field types, which may refer back to this type.
    public Layout Full(TT t)
    {
        var L = Of(t);
        if (L.Error != null) throw new EmitError(L.Error);
        if (L.Done) return L;
        L.Done = true;
        try { return FullInner(t, L); }
        catch (Exception e) when (e is not EmitError || L.Error == null)
        {
            L.Error = $"layout of {t.Key}: {e.Message}";
            if (Environment.GetEnvironmentVariable("NR_TRACE") != null) Console.Error.WriteLine(L.Error + Environment.NewLine + e.StackTrace);
            throw new EmitError(L.Error);
        }
    }

    Layout FullInner(TT t, Layout L)
    {
        if (t.K is TK.SzArray or TK.Array) { ArrayDispatch(t, L); return L; }
        if (t.K != TK.Def || t.IsOpen) return L;

        var bt = w.BaseOf(t);
        Layout? B = bt != null ? Full(bt) : null;
        int depth = 0;
        for (var x = bt; x != null; x = w.BaseOf(x)) depth++;
        if (B != null && !t.IsValueType) L.Inst.AddRange(B.Inst);
        var taken = new HashSet<string>(L.Inst.Select(i => i.cname));
        foreach (var f in t.Def.Fields)
        {
            if (f.IsLiteral) continue;
            var fi = new FI { Owner = t, Def = f, Type = w.FieldType(t, f) };
            var n = $"f{depth}_{San(f.Name)}";
            while (!taken.Add(n)) n += "_";
            L.FieldName[f] = n;
            (f.IsStatic ? L.Statics : L.Inst).Add((fi, n));
        }

        if (t.IsInterface)
        {
            foreach (var m in t.Def.Methods)
                if (m.IsVirtual && !m.HasGenericParameters) { L.SlotOf[m] = L.IfaceMethods.Count; L.IfaceMethods.Add(m); }
            return L;
        }

        // Class vtable: inherit, then override by name+signature or append.
        if (B != null) L.Slots.AddRange(B.Slots);
        foreach (var m in t.Def.Methods)
        {
            if (!m.IsVirtual || m.HasGenericParameters) continue;
            var mi = w.Method(t, m, Array.Empty<TT>());
            int s = -1;
            if (!m.IsNewSlot)
                for (int i = L.Slots.Count - 1; i >= 0; i--)
                    if (L.Slots[i] is MI o && o.Def.Name == m.Name && SameSig(o, mi)) { s = i; break; }
            if (s < 0) { s = L.Slots.Count; L.Slots.Add(mi); } else L.Slots[s] = mi;
            L.SlotOf[m] = s;
        }
        foreach (var m in t.Def.Methods)
            foreach (var ov in m.Overrides)
            {
                var target = w.ConvMethod(ov, t.Args, Array.Empty<TT>());
                if (target == null || target.Owner.IsInterface || target.Def.HasGenericParameters) continue;
                var TL = Full(target.Owner);
                if (TL.SlotOf.TryGetValue(target.Def, out var s)) L.Slots[s] = w.Method(t, m, Array.Empty<TT>());
            }

        // Interface map: inherited mappings keep the base's slots unless this
        // type re-declares the interface (ECMA-335 II.12.2).
        var declared = new HashSet<TT>();
        foreach (var ii in t.Def.Interfaces) { var it = w.Conv(ii.InterfaceType, t.Args, Array.Empty<TT>()); declared.Add(it); foreach (var x in AllIfaces(it)) declared.Add(x); }
        var order = new List<TT>();
        if (B != null) order.AddRange(B.IfaceSlots.Keys);
        foreach (var it in declared) if (!order.Contains(it)) order.Add(it);
        foreach (var it in order)
        {
            if (!declared.Contains(it) && B != null && B.IfaceSlots.TryGetValue(it, out var inh)) { L.IfaceSlots[it] = inh; continue; }
            var IL = Full(it);
            var map = new int[IL.IfaceMethods.Count];
            for (int k = 0; k < map.Length; k++) map[k] = FindImpl(t, it, IL.IfaceMethods[k]);
            L.IfaceSlots[it] = map;
        }
        foreach (var kv in L.IfaceSlots)
            L.Ifaces.Add((kv.Key, kv.Value.Select(s => s >= 0 ? L.Slots[s] : null).ToArray()));
        return L;
    }

    public IEnumerable<TT> AllIfaces(TT it)
    {
        foreach (var ii in it.Def.Interfaces)
        {
            var x = w.Conv(ii.InterfaceType, it.Args, Array.Empty<TT>());
            yield return x;
            foreach (var y in AllIfaces(x)) yield return y;
        }
    }

    int FindImpl(TT t, TT iface, MethodDefinition im)
    {
        var want = w.Method(iface, im, Array.Empty<TT>());
        for (var c = t; c != null; c = w.BaseOf(c))
        {
            var CL = Full(c);
            foreach (var m in c.Def.Methods)
                foreach (var ov in m.Overrides)
                {
                    var target = w.ConvMethod(ov, c.Args, Array.Empty<TT>());
                    if (target != null && target.Owner == iface && target.Def == im && CL.SlotOf.TryGetValue(m, out var s)) return s;
                }
            foreach (var m in c.Def.Methods)
                if (m.IsVirtual && m.Name == im.Name && !m.HasGenericParameters && CL.SlotOf.TryGetValue(m, out var s)
                    && SameSig(w.Method(c, m, Array.Empty<TT>()), want)) return s;
        }
        return -1;
    }

    public bool SameSig(MI a, MI b)
    {
        if (a.Def.Parameters.Count != b.Def.Parameters.Count) return false;
        if (a.Def.GenericParameters.Count != b.Def.GenericParameters.Count) return false;
        try
        {
            if (w.RetType(a) != w.RetType(b)) return false;
            for (int i = 0; i < a.Def.Parameters.Count; i++)
                if (w.ParamType(a, i) != w.ParamType(b, i)) return false;
        }
        catch (UnresolvedGeneric) { return SigText(a.Def) == SigText(b.Def); }
        return true;
    }

    static string SigText(MethodDefinition m) =>
        m.ReturnType.FullName + "(" + string.Join(",", m.Parameters.Select(p => p.ParameterType.FullName)) + ")";

    // T[] gets System.Array's vtable, and for vectors IList<T>, ICollection<T>
    // and IEnumerable<T> implemented by Array.InternalArray__* instantiated at T.
    void ArrayDispatch(TT t, Layout L)
    {
        var arr = w.Cor("System", "Array");
        var AL = Full(arr);
        L.Slots.AddRange(AL.Slots);
        foreach (var x in AL.Ifaces) L.Ifaces.Add(x);
        foreach (var kv in AL.IfaceSlots) L.IfaceSlots[kv.Key] = kv.Value;
        if (t.K != TK.SzArray) return;
        var gen = new[] { "IList`1", "ICollection`1", "IEnumerable`1" }
            .Select(n => w.Inst(w.CorDef("System.Collections.Generic", n), new[] { t.Elem }));
        foreach (var it in gen)
        {
            var IL = Full(it);
            var impls = new MI?[IL.IfaceMethods.Count];
            for (int k = 0; k < impls.Length; k++)
            {
                var im = IL.IfaceMethods[k];
                var n = im.Name;
                // IList<T>.get_Item -> InternalArray__get_Item; ICollection<T>.Add -> InternalArray__ICollection_Add
                var tn = it.Def.Name.StartsWith("ICollection") ? "ICollection_" + n
                       : it.Def.Name.StartsWith("IEnumerable") ? "IEnumerable_" + n : n;
                var md = arr.Def.Methods.FirstOrDefault(m => m.Name == "InternalArray__" + tn && m.Parameters.Count == im.Parameters.Count);
                impls[k] = md == null ? null : w.Method(arr, md, new[] { t.Elem });
            }
            L.Ifaces.Add((it, impls));
        }
    }
}
