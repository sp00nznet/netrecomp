// Whole-program driver: reachability (rapid type analysis), method lowering,
// and the generated C tree (nrgen.h, m_*.c, meta.c).
//
// A method is compiled when something calls it, or when it sits in the vtable
// of a type that is ever constructed. Root assemblies (the game's own code and
// UnityEngine) are compiled whole, because Unity's native side finds and
// constructs their types by name at run time.
using System.Text;
using Mono.Cecil;
using Mono.Cecil.Cil;

sealed class Gen
{
    public readonly World W;
    public readonly Layouts Lay;
    readonly Queue<MI> queue = new();
    readonly HashSet<MI> reached = new();
    readonly List<MI> order = new();
    readonly HashSet<TT> constructed = new();
    readonly List<TT> constructedList = new();
    readonly HashSet<TT> klassUsed = new();
    readonly HashSet<TT> staticsUsed = new();
    readonly HashSet<TT> objUsed = new();
    readonly Dictionary<MI, string> bodies = new();
    readonly Dictionary<string, int> strs = new();
    readonly List<string> strList = new();
    readonly Dictionary<MI, int> gvms = new();           // generic virtual call sites -> table id
    readonly HashSet<MI> thunks = new();
    readonly HashSet<MI> descs = new();
    readonly Dictionary<FieldDefinition, (FI f, int id)> fhandles = new();
    readonly HashSet<FI> rvas = new();
    public readonly Dictionary<string, int> Failures = new();
    public readonly List<string> FailedMethods = new();
    public int Ok, Icalls, Pinvokes, PinvokeFail;
    public readonly SortedSet<string> IcallKeys = new(StringComparer.Ordinal);
    public MI? Entry;
    int nextMid;

    public readonly Attrs At;
    public Gen(World w) { W = w; Lay = new Layouts(w); At = new Attrs(this); }

    // ---- naming ----
    static string San(string s, int max = 48)
    {
        var b = new StringBuilder();
        foreach (var ch in s) b.Append(char.IsAsciiLetterOrDigit(ch) ? ch : '_');
        return b.Length > max ? b.ToString(0, max) : b.ToString();
    }
    string Name(MI m)
    {
        if (m.Id < 0) { m.Id = nextMid++; m.CName = $"M{m.Id}_{San(m.Owner.Def.Name, 24)}_{San(m.Def.Name, 24)}"; }
        return m.CName;
    }

    public string Klass(TT t) { klassUsed.Add(t); return "K" + Lay.Id(t); }
    public string ObjStruct(TT t) { objUsed.Add(t); Lay.Full(t); return "O" + Lay.Id(t); }
    public string Statics(TT t) { staticsUsed.Add(t); Lay.Full(t); Klass(t); return "S" + Lay.Id(t); }
    public string FieldC(FI f) { var L = Lay.Full(f.Owner); if (!L.FieldName.TryGetValue(f.Def, out var n)) throw new Exception($"field {f.Def} of {f.Owner.Key}: owner def same={f.Owner.Def == f.Def.DeclaringType} names={string.Join(",", L.FieldName.Keys.Select(k => k.Name))} done={L.Done} kind={f.Owner.K}"); return n; }

    public string Str(string s)
    {
        if (!strs.TryGetValue(s, out var i)) { i = strList.Count; strs[s] = i; strList.Add(s); }
        return $"NR_STR({i})";
    }

    public string Fn(MI m) { Need(m); return Name(m); }
    public string FnPtr(MI m)
    {
        Need(m);
        if (m.Owner.IsValueType && m.Def.HasThis) { thunks.Add(m); return "U" + Name(m)[1..]; }
        return Name(m);
    }

    public string Proto(MI m, string? name = null)
    {
        var ps = Params(m).Select((c, i) => $"{c} a{i}").ToList();
        return $"{Lay.C(W.RetType(m))} {name ?? Name(m)}({(ps.Count == 0 ? "void" : string.Join(", ", ps))})";
    }
    public List<string> Params(MI m)
    {
        var l = new List<string>();
        if (m.Def.HasThis) l.Add("void*");
        for (int i = 0; i < m.Def.Parameters.Count; i++) l.Add(Lay.C(W.ParamType(m, i)));
        return l;
    }
    public string FnPtrType(MI m) => $"{Lay.C(W.RetType(m))} (*)({string.Join(", ", Params(m).DefaultIfEmpty("void"))})";

    // ---- reachability ----
    public void Need(MI m)
    {
        if (!reached.Add(m)) return;
        Name(m);
        queue.Enqueue(m);
        AotHints(m);
    }

    // Instantiations the class library makes by reflection (MakeGenericType on
    // a typeof(X<>) literal), which no IL scan can see.
    void AotHints(MI m)
    {
        if (!m.Def.IsConstructor || !m.Def.IsStatic || m.Owner.Args.Length != 1) return;
        var o = m.Owner.Def;
        if (o.Module.Assembly.Name.Name != "mscorlib" || o.Namespace != "System.Collections.Generic") return;
        var t = m.Owner.Args[0];
        (string iface, string impl) = o.Name switch
        {
            "EqualityComparer`1" => ("IEquatable`1", "GenericEqualityComparer`1"),
            "Comparer`1" => ("IComparable`1", "GenericComparer`1"),
            _ => ("", ""),
        };
        if (impl == "") return;
        var it = W.Inst(W.CorDef("System", iface), new[] { t });
        if (t.K == TK.Def && !t.IsInterface && Lay.Full(t).IfaceSlots.ContainsKey(it))
        {
            var ct = W.Inst(W.CorDef("System.Collections.Generic", impl), new[] { t });
            Construct(ct);
            foreach (var c in ct.Def.Methods.Where(x => x.IsConstructor && !x.IsStatic)) Need(W.Method(ct, c, Array.Empty<TT>()));
        }
    }

    public void Construct(TT t)
    {
        if (t.K is TK.Ptr or TK.ByRef or TK.FnPtr) return;
        if (!constructed.Add(t)) return;
        constructedList.Add(t);
        Klass(t);
        var L = Lay.Full(t);
        if (t.K is TK.SzArray or TK.Array) Construct(t.Elem.IsValueType ? t.Elem : t.Elem);
        foreach (var s in L.Slots) if (s != null && !s.Def.IsAbstract) Need(s);
        foreach (var (_, impls) in L.Ifaces) foreach (var s in impls) if (s != null && !s.Def.IsAbstract) Need(s);
        if (t.K == TK.Def)
        {
            var cc = t.Def.Methods.FirstOrDefault(x => x.IsConstructor && x.IsStatic);
            if (cc != null) Need(W.Method(t, cc, Array.Empty<TT>()));
        }
    }

    public bool HasCctor(TT t) => t.K == TK.Def && !t.IsOpen && t.Def.Methods.Any(x => x.IsConstructor && x.IsStatic);
    // The class record for an NR_CCTOR check; compiles the cctor it will run.
    public string Cctor(TT t)
    {
        Need(W.Method(t, t.Def.Methods.First(x => x.IsConstructor && x.IsStatic), Array.Empty<TT>()));
        return Klass(t);
    }
    // Same-class accesses rely on the entry check, which only precise (not
    // beforefieldinit) types get; the cctor itself never checks.
    public bool NeedsCctorCheck(TT owner, MI from) => HasCctor(owner)
        && (owner != from.Owner || (owner.Def.IsBeforeFieldInit && !(from.Def.IsConstructor && from.Def.IsStatic)));
    public bool CctorCheckOnEntry(MI m) => HasCctor(m.Owner) && !m.Owner.Def.IsBeforeFieldInit
        && (m.Def.IsStatic || m.Def.IsConstructor) && !(m.Def.IsConstructor && m.Def.IsStatic);

    public int Slot(MI m)
    {
        var L = Lay.Full(m.Owner);
        if (!L.SlotOf.TryGetValue(m.Def, out var s)) throw new EmitError("no slot for " + m.Key);
        return s;
    }
    public int IfaceIndex(MI m)
    {
        Klass(m.Owner);
        var L = Lay.Full(m.Owner);
        if (!L.SlotOf.TryGetValue(m.Def, out var s)) throw new EmitError("no iface slot for " + m.Key);
        return s;
    }

    public string Gvm(MI m)
    {
        if (!gvms.TryGetValue(m, out var id)) { id = gvms.Count; gvms[m] = id; }
        return "G" + id;
    }

    public string MethodDesc(MI m) { Need(m); descs.Add(m); Klass(m.Owner); return "MD" + Name(m)[1..]; }
    public string FieldHandle(FI f)
    {
        if (!fhandles.TryGetValue(f.Def, out var x)) { x = (f, fhandles.Count); fhandles[f.Def] = x; }
        if (f.Def.HasFieldRVA) rvas.Add(f);
        Klass(f.Owner);
        return "FD" + x.id;
    }
    public string Rva(FI f) { FieldHandle(f); return $"RVA{fhandles[f.Def].id}"; }

    public bool IsDelegate(TT t) => t.K == TK.Def && t.Def.BaseType?.FullName == "System.MulticastDelegate";
    public bool IsNullable(TT t) => t.K == TK.Def && t.Def.FullName == "System.Nullable`1";
    public string NullableHasValueOffset(TT t) { ObjStructValue(t); return $"offsetof({Lay.C(t)}, {Lay.Full(t).FieldName[t.Def.Fields.First(f => f.Name is "has_value" or "hasValue")]})"; }
    public string NullableValueOffset(TT t) => $"offsetof({Lay.C(t)}, {Lay.Full(t).FieldName[t.Def.Fields.First(f => f.Name == "value")]})";
    void ObjStructValue(TT t) => Lay.Full(t);

    // newobj System.String::.ctor(X) -> String::CreateString(X) on a null this.
    public MI StringCreate(MI ctor)
    {
        var d = ctor.Owner.Def.Methods.First(x => x.Name == "CreateString" && x.Parameters.Count == ctor.Def.Parameters.Count
            && x.Parameters.Select(p => p.ParameterType.FullName).SequenceEqual(ctor.Def.Parameters.Select(p => p.ParameterType.FullName)));
        return W.Method(ctor.Owner, d, Array.Empty<TT>());
    }

    // The implementation of decl on value type vt, if vt itself provides it.
    public MI? ValueTypeImpl(TT vt, MI decl)
    {
        var L = Lay.Full(vt);
        MI? impl = null;
        if (decl.Owner.IsInterface)
        {
            if (L.IfaceSlots.TryGetValue(decl.Owner, out var map) && Lay.Full(decl.Owner).SlotOf.TryGetValue(decl.Def, out var k) && map[k] >= 0)
                impl = L.Slots[map[k]];
        }
        else if (decl.Def.IsVirtual && Lay.Full(decl.Owner).SlotOf.TryGetValue(decl.Def, out var s)) impl = L.Slots[s];
        else if (decl.Owner == vt) impl = decl;
        if (decl.Def.HasGenericParameters && impl == null && decl.Owner == vt) impl = decl;
        if (impl != null && impl.Owner == vt)
            return decl.MArgs.Length > 0 ? W.Method(vt, impl.Def, decl.MArgs) : impl;
        return null;
    }

    // ---- driver ----
    public void Root(MI m) => Need(m);

    public void Run()
    {
        int gvmSeen = 0;
        while (true)
        {
            while (queue.Count > 0)
            {
                var m = queue.Dequeue();
                order.Add(m);
                bodies[m] = Body(m);
            }
            // Generic virtual methods: every constructed type that overrides a
            // called GVM gets that override instantiated at the same arguments.
            bool more = false;
            foreach (var g in gvms.Keys.ToList())
                foreach (var t in constructedList.ToList())
                {
                    var impl = GvmImpl(t, g);
                    if (impl != null && !reached.Contains(impl)) { Need(impl); more = true; }
                }
            more |= At.Scan(klassUsed.Concat(constructed));
            if (!more && queue.Count == 0 && gvms.Count == gvmSeen) break;
            gvmSeen = gvms.Count;
        }
    }

    public MI? GvmImpl(TT t, MI decl)
    {
        if (t.K != TK.Def || t.IsInterface) return null;
        if (!Subtype(t, decl.Owner)) return null;
        for (var c = t; c != null; c = W.BaseOf(c))
        {
            if (c.K != TK.Def) break;
            foreach (var md in c.Def.Methods)
            {
                if (!md.HasGenericParameters || md.GenericParameters.Count != decl.MArgs.Length) continue;
                bool explicitHit = md.Overrides.Any(ov => { var x = W.ConvMethod(ov, c.Args, decl.MArgs); return x != null && x.Owner == decl.Owner && x.Def == decl.Def; });
                bool nameHit = md.IsVirtual && md.Name == decl.Def.Name && md.Parameters.Count == decl.Def.Parameters.Count
                    && SigText(md) == SigText(decl.Def);
                if (explicitHit || nameHit) return md.IsAbstract ? null : W.Method(c, md, decl.MArgs);
            }
        }
        return null;
    }
    static string SigText(MethodDefinition m) => m.ReturnType.Name + "(" + string.Join(",", m.Parameters.Select(p => p.ParameterType.Name)) + ")";

    bool Subtype(TT t, TT of)
    {
        for (var c = t; c != null; c = W.BaseOf(c)) if (c == of) return true;
        return of.IsInterface && Lay.Full(t).IfaceSlots.ContainsKey(of);
    }

    string Body(MI m)
    {
        var d = m.Def;
        try
        {
            if (d.IsInternalCall) { Icalls++; Ok++; IcallKeys.Add(TypeKeyName(m.Owner.Def) + "::" + m.Def.Name + "(" + string.Join(",", m.Def.Parameters.Select(p => SigName(p.ParameterType))) + ")"); return Icall(m); }
            if (d.IsPInvokeImpl) { Pinvokes++; var s = PInvoke(m); Ok++; return s; }
            if (d.IsRuntime && IsDelegate(m.Owner)) { Ok++; return DelegateMethod(m); }
            if (!d.HasBody) { if (d.IsAbstract) return ""; throw new EmitError("no body"); }
            var c = new MethodEmitter(this, m).Emit();
            Ok++;
            return c;
        }
        catch (Exception e) when (e is EmitError or UnresolvedGeneric or NotSupportedException or InvalidOperationException)
        {
            if (e is not EmitError && Environment.GetEnvironmentVariable("NR_TRACE") != null) Console.Error.WriteLine($"{m.Key}: {e}");
            var why = e.Message.Split(" at ")[0];
            if (why.StartsWith("stack ")) why = why.Split(':')[0];
            Failures[why] = Failures.GetValueOrDefault(why) + 1;
            FailedMethods.Add($"{m.Key}\t{e.Message}");
            if (d.IsPInvokeImpl) PinvokeFail++;
            return $"{Proto(m)}\n{{\n    nr_throw_notsupported(\"{CEsc(m.Key)}: {CEsc(e.Message)}\");\n{(Layouts.IsVoid(W.RetType(m)) ? "" : $"    {{ {Lay.C(W.RetType(m))} z; memset(&z, 0, sizeof z); return z; }}\n")}}}\n";
        }
    }

    public static string CEsc(string s)
    {
        var b = new StringBuilder();
        foreach (var ch in s)
        {
            if (ch == '"' || ch == '\\') b.Append('\\').Append(ch);
            else if (ch < 32 || ch > 126) b.Append($"\\x{(int)ch:x2}\"\"");
            else if (ch == '?') b.Append("\\?");
            else b.Append(ch);
        }
        return b.ToString();
    }

    // mono's icall key: "Ns.Type::Method" and "Ns.Type::Method(sig)".
    public static string TypeKeyName(TypeDefinition t) =>
        t.DeclaringType != null ? TypeKeyName(t.DeclaringType) + "/" + t.Name : (t.Namespace.Length > 0 ? t.Namespace + "." : "") + t.Name;
    static readonly Dictionary<string, string> shortNames = new()
    {
        ["System.Boolean"] = "bool", ["System.Char"] = "char", ["System.SByte"] = "sbyte", ["System.Byte"] = "byte",
        ["System.Int16"] = "int16", ["System.UInt16"] = "uint16", ["System.Int32"] = "int", ["System.UInt32"] = "uint",
        ["System.Int64"] = "long", ["System.UInt64"] = "ulong", ["System.Single"] = "single", ["System.Double"] = "double",
        ["System.String"] = "string", ["System.Object"] = "object", ["System.IntPtr"] = "intptr", ["System.UIntPtr"] = "uintptr",
        ["System.Void"] = "void",
    };
    static string SigName(TypeReference t) => t switch
    {
        ByReferenceType b => SigName(b.ElementType) + "&",
        ArrayType a => SigName(a.ElementType) + "[" + new string(',', a.Rank - 1) + "]",
        PointerType p => SigName(p.ElementType) + "*",
        GenericInstanceType g => SigName(g.ElementType) + "<" + string.Join(",", g.GenericArguments.Select(SigName)) + ">",
        GenericParameter gp => gp.Name,
        _ => shortNames.TryGetValue(t.FullName, out var s) ? s : (t.Resolve() is TypeDefinition d ? TypeKeyName(d) : t.FullName),
    };

    string Icall(MI m)
    {
        var key = TypeKeyName(m.Owner.Def) + "::" + m.Def.Name;
        var full = key + "(" + string.Join(",", m.Def.Parameters.Select(p => SigName(p.ParameterType))) + ")";
        var args = string.Join(", ", Params(m).Select((_, i) => $"a{i}"));
        var rt = Lay.C(W.RetType(m));
        var ft = $"{rt} (*)({string.Join(", ", Params(m).DefaultIfEmpty("void"))})";
        var genericArg = m.MArgs.Length > 0 ? $"(NrClass*)&{Klass(m.MArgs[0])}" : "0";
        // Generic icalls (Array.GetGenericValueImpl<T>) get T's class as a trailing hidden argument.
        if (m.MArgs.Length > 0)
            return $"{Proto(m)}\n{{\n    static void* fn;\n    if (!fn) fn = nr_icall(\"{CEsc(full)}\", \"{CEsc(key)}\");\n    {(rt == "void" ? "" : "return ")}(({rt} (*)({string.Join(", ", Params(m).Append("NrClass*"))}))fn)({string.Join(", ", Params(m).Select((_, i) => $"a{i}").Append(genericArg))});\n}}\n";
        return $"{Proto(m)}\n{{\n    static void* fn;\n    if (!fn) fn = nr_icall(\"{CEsc(full)}\", \"{CEsc(key)}\");\n    {(rt == "void" ? "" : "return ")}(({ft})fn)({args});\n}}\n";
    }

    // P/Invoke: blittable arguments pass through; strings, bools and arrays
    // of blittables are marshalled. Anything else is reported, not guessed.
    string PInvoke(MI m)
    {
        var pi = m.Def.PInvokeInfo;
        var lib = pi.Module.Name;
        var entry = string.IsNullOrEmpty(pi.EntryPoint) ? m.Def.Name : pi.EntryPoint;
        bool unicode = pi.IsCharSetUnicode;
        var cc = pi.IsCallConvCdecl ? "__cdecl" : "__stdcall";
        var pre = new StringBuilder(); var post = new StringBuilder();
        var nparams = new List<string>(); var nargs = new List<string>();
        for (int i = 0; i < m.Def.Parameters.Count; i++)
        {
            var p = m.Def.Parameters[i];
            var t = W.ParamType(m, i);
            var a = $"a{i}";
            var mt = p.MarshalInfo?.NativeType;
            if (t.K == TK.Def && t.Def.FullName == "System.String")
            {
                bool wide = mt == NativeType.LPWStr || (unicode && mt != NativeType.LPStr);
                nparams.Add(wide ? "const uint16_t*" : "const char*");
                if (wide) nargs.Add($"nr_str_chars({a})");
                else { pre.Append($"    char* t{i} = nr_str_to_utf8({a});\n"); nargs.Add($"t{i}"); post.Append($"    nr_free(t{i});\n"); }
            }
            else if (t.K == TK.Def && t.Def.FullName == "System.Boolean")
            {
                if (mt is NativeType.I1 or NativeType.U1) { nparams.Add("uint8_t"); nargs.Add(a); }
                else { nparams.Add("int32_t"); nargs.Add($"(int32_t){a}"); }
            }
            else if (t.K == TK.ByRef && t.Elem.K == TK.Def && t.Elem.Def.FullName == "System.Boolean")
            {
                // bool& marshals as a 4-byte BOOL unless declared I1/U1
                if (mt is NativeType.I1 or NativeType.U1) { nparams.Add("void*"); nargs.Add(a); }
                else
                {
                    pre.Append($"    int32_t t{i} = *(uint8_t*){a};\n");
                    nparams.Add("int32_t*"); nargs.Add($"&t{i}");
                    post.Append($"    *(uint8_t*){a} = t{i} != 0;\n");
                }
            }
            else if (t.K == TK.SzArray && Blittable(t.Elem)) { nparams.Add("void*"); nargs.Add($"({a} ? NR_ADATA({a}) : 0)"); }
            else if (t.K == TK.ByRef && Blittable(t.Elem)) { nparams.Add("void*"); nargs.Add(a); }
            else if (Blittable(t)) { nparams.Add(Lay.C(t)); nargs.Add(a); }
            else if (IsDelegate(t)) { nparams.Add("void*"); nargs.Add($"nr_delegate_to_fnptr({a})"); }
            else throw new EmitError("pinvoke param " + t.Key);
        }
        var rt = W.RetType(m);
        string nrt, conv;
        if (Layouts.IsVoid(rt)) { nrt = "void"; conv = ""; }
        else if (rt.K == TK.Def && rt.Def.FullName == "System.Boolean")
        {
            var mt = m.Def.MethodReturnType.MarshalInfo?.NativeType;
            nrt = mt is NativeType.I1 or NativeType.U1 ? "uint8_t" : "int32_t"; conv = "r != 0";
        }
        else if (rt.K == TK.Def && rt.Def.FullName == "System.String") { nrt = unicode ? "const uint16_t*" : "const char*"; conv = unicode ? "nr_str_from_utf16z(r)" : "nr_str_from_utf8(r)"; }
        else if (Blittable(rt)) { nrt = Lay.C(rt); conv = "r"; }
        else throw new EmitError("pinvoke return " + rt.Key);
        var sb = new StringBuilder();
        sb.Append(Proto(m)).Append("\n{\n");
        sb.Append($"    static {nrt} ({cc} *fn)({string.Join(", ", nparams.DefaultIfEmpty("void"))});\n");
        sb.Append($"    if (!fn) *(void**)&fn = nr_pinvoke(\"{CEsc(lib)}\", \"{CEsc(entry)}\");\n");
        sb.Append(pre);
        var call = $"fn({string.Join(", ", nargs)})";
        if (nrt == "void") sb.Append($"    {call};\n").Append(post);
        else sb.Append($"    {nrt} r = {call};\n").Append(post).Append($"    return {conv};\n");
        sb.Append("}\n");
        return sb.ToString();
    }

    bool Blittable(TT t)
    {
        if (t.K is TK.Ptr or TK.FnPtr) return true;
        if (t.K != TK.Def || !t.IsValueType) return false;
        if (Layouts.IsPrim(t)) return t.Def.FullName is not ("System.Boolean" or "System.Char");
        if (t.IsEnum) return true;
        return Lay.Full(t).Inst.All(f => Blittable(f.f.Type));
    }

    // Delegate Invoke: the prev chain first (mono's multicast list), then this
    // target. A non-null target is passed as the first argument, which covers
    // instance methods and closed static ones alike.
    string DelegateMethod(MI m)
    {
        var n = m.Def.Name;
        if (n == "BeginInvoke") return BeginInvoke(m);
        if (n != "Invoke")
            return $"{Proto(m)}\n{{\n    nr_throw_notsupported(\"{CEsc(m.Key)}\");\n{(Layouts.IsVoid(W.RetType(m)) ? "" : $"    {{ {Lay.C(W.RetType(m))} z; memset(&z, 0, sizeof z); return z; }}\n")}}}\n";
        var ps = Params(m).Skip(1).ToList();
        var args = ps.Select((_, i) => $"a{i + 1}").ToList();
        var rt = Lay.C(W.RetType(m));
        var ret = rt == "void" ? "" : "return ";
        var sb = new StringBuilder();
        sb.Append(Proto(m)).Append("\n{\n    NrDelegate* d = (NrDelegate*)a0;\n");
        sb.Append($"    if (d->prev) {Name(m)}({string.Join(", ", args.Prepend("d->prev"))});\n");
        sb.Append($"    if (d->target) {ret}(({rt} (*)({string.Join(", ", ps.Prepend("void*"))}))d->method_ptr)({string.Join(", ", args.Prepend("d->target"))});\n");
        sb.Append($"    else {ret}(({rt} (*)({string.Join(", ", ps.DefaultIfEmpty("void"))}))d->method_ptr)({string.Join(", ", args)});\n");
        sb.Append("}\n");
        return sb.ToString();
    }

    // BeginInvoke runs Invoke on a new thread (mono 2.x's ThreadPool is built
    // on it). ponytail: no IAsyncResult comes back and the callback gets null,
    // so EndInvoke can't work; fine for fire-and-forget, which is all the
    // class library's ThreadPool needs. A real AsyncResult if a title calls EndInvoke.
    string BeginInvoke(MI m)
    {
        var inv = W.Method(m.Owner, m.Owner.Def.Methods.First(x => x.Name == "Invoke"), Array.Empty<TT>());
        Need(inv);
        var ips = Params(inv).Skip(1).ToList();
        var id = Name(m)[1..];
        var sb = new StringBuilder();
        sb.Append($"typedef struct BI{id} {{ void* d; void* cb; {string.Concat(ips.Select((c, i) => $"{c} a{i + 1}; "))}}} BI{id};\n");
        sb.Append($"static void BW{id}(void* p)\n{{\n    BI{id}* c = (BI{id}*)p;\n");
        sb.Append($"    {Name(inv)}({string.Join(", ", ips.Select((_, i) => $"c->a{i + 1}").Prepend("c->d"))});\n");
        sb.Append("    if (c->cb) nr_async_callback(c->cb);\n}\n");
        sb.Append(Proto(m)).Append("\n{\n");
        sb.Append($"    BI{id}* c = (BI{id}*)nr_alloc_root(sizeof(BI{id}));\n    c->d = a0;\n");
        for (int i = 0; i < ips.Count; i++) sb.Append($"    c->a{i + 1} = a{i + 1};\n");
        sb.Append($"    c->cb = a{ips.Count + 1};\n    nr_async_start(BW{id}, c);\n    return 0;\n}}\n");
        return sb.ToString();
    }

    // ---- output ----
    public void Write(string dir, int perFile = 1500)
    {
        Directory.CreateDirectory(dir);
        foreach (var f in Directory.GetFiles(dir, "m_*.c")) File.Delete(f);
        // Close the class set over parents, elements, interfaces and vtables.
        var meta = new MetaWriter(this);
        meta.Close();
        File.WriteAllText(Path.Combine(dir, "nrgen.h"), meta.Header());
        int file = 0;
        for (int i = 0; i < order.Count; i += perFile)
        {
            var sb = new StringBuilder("#include \"nrgen.h\"\n\n");
            foreach (var m in order.Skip(i).Take(perFile))
                if (bodies[m].Length > 0) sb.Append("// ").Append(m.Key.Replace("\n", " ").Replace("*/", "* /")).Append('\n').Append(bodies[m]).Append('\n');
            File.WriteAllText(Path.Combine(dir, $"m_{file++:d3}.c"), sb.ToString());
        }
        var mc = meta.Body();
        if (Entry != null)
        {
            var args = Entry.Def.Parameters.Count == 1 ? "a" : "";
            var call = $"{NameOf(Entry)}({args})";
            mc += $"\nint32_t nr_entry(void* a) {{ (void)a; {(Layouts.IsVoid(W.RetType(Entry)) ? call + "; return 0;" : "return " + call + ";")} }}\n";
        }
        else mc += "\nint32_t nr_entry(void* a) { (void)a; return -1; }\n";
        File.WriteAllText(Path.Combine(dir, "meta.c"), mc);
        File.WriteAllLines(Path.Combine(dir, "icalls.txt"), IcallKeys);
        File.WriteAllText(Path.Combine(dir, "chartab.c"), CharTables());
        File.WriteAllText(Path.Combine(dir, "fmttab.c"), FormatterTables());
    }

    // System.Char's lookup tables (mono's runtime supplied these), built from
    // the Unicode data in this tool's own runtime.
    static string CharTables()
    {
        var sb = new StringBuilder("/* generated by netrecomp from Unicode data */\n#include <stdint.h>\n");
        var nums = new List<double> { -1 };
        var cat = new byte[65536]; var num = new byte[65536];
        for (int i = 0; i < 65536; i++)
        {
            var c = (char)i;
            cat[i] = (byte)System.Globalization.CharUnicodeInfo.GetUnicodeCategory(c);
            var v = System.Globalization.CharUnicodeInfo.GetNumericValue(c);
            int k = nums.IndexOf(v);
            if (k < 0) { k = nums.Count; nums.Add(v); }
            num[i] = (byte)k;
        }
        void Bytes(string name, byte[] b) => sb.Append($"const uint8_t {name}[65536] = {{{string.Join(",", b)}}};\n");
        void Chars(string name, int from, int to, Func<char, char> f) =>
            sb.Append($"const uint16_t {name}[] = {{{string.Join(",", Enumerable.Range(from, to - from + 1).Select(i => (int)f((char)i)))}}};\n");
        Bytes("nr_char_category", cat);
        Bytes("nr_char_numeric", num);
        sb.Append($"const double nr_char_numeric_values[] = {{{string.Join(",", nums.Select(d => d.ToString("R", System.Globalization.CultureInfo.InvariantCulture) + (d % 1 == 0 ? ".0" : "")))}}};\n");
        Chars("nr_to_lower_low", 0, 0x24CF, char.ToLowerInvariant);
        Chars("nr_to_lower_high", 0xFF21, 0xFFFF, char.ToLowerInvariant);
        Chars("nr_to_upper_low", 0, 0x24E9, char.ToUpperInvariant);
        Chars("nr_to_upper_high", 0xFF21, 0xFFFF, char.ToUpperInvariant);
        return sb.ToString();
    }

    // NumberFormatter's tables (mono's runtime supplied these). Entry e of the
    // mantissa table is 2^(e-1075) / 10^K as a 64-bit fraction, with K chosen
    // so the fraction lands in [2^64/10, 2^64); K is the exponent table.
    static string FormatterTables()
    {
        var sb = new StringBuilder("/* generated by netrecomp */\n#include <stdint.h>\n");
        var mant = new ulong[2048]; var tens = new int[2048];
        var two64 = System.Numerics.BigInteger.One << 64;
        for (int e = 1; e < 2048; e++)
        {
            int p = e - 1075;
            int k = (int)Math.Floor(p * Math.Log10(2)) + 1;
            System.Numerics.BigInteger num = two64, den = 1;
            if (p >= 0) num <<= p; else den <<= -p;
            if (k >= 0) den *= System.Numerics.BigInteger.Pow(10, k); else num *= System.Numerics.BigInteger.Pow(10, -k);
            var q = (num + den / 2) / den;
            while (q >= two64) { k++; den *= 10; q = (num + den / 2) / den; }
            while (q * 10 < two64) { k--; num *= 10; q = (num + den / 2) / den; }
            mant[e] = (ulong)q; tens[e] = k;
        }
        mant[0] = mant[1]; tens[0] = tens[1];
        sb.Append($"const uint64_t nr_fmt_mantissa[2048] = {{{string.Join(",", mant.Select(v => v + "ull"))}}};\n");
        sb.Append($"const int32_t nr_fmt_tens[2048] = {{{string.Join(",", tens)}}};\n");
        sb.Append("const uint16_t nr_fmt_digit_lower[16] = {'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};\n");
        sb.Append("const uint16_t nr_fmt_digit_upper[16] = {'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'};\n");
        sb.Append($"const int64_t nr_fmt_ten_powers[19] = {{{string.Join(",", Enumerable.Range(0, 19).Select(i => System.Numerics.BigInteger.Pow(10, i) + "ll"))}}};\n");
        sb.Append($"const int32_t nr_fmt_dec_hex[100] = {{{string.Join(",", Enumerable.Range(0, 100).Select(i => (i / 10) * 16 + i % 10))}}};\n");
        return sb.ToString();
    }

    // Accessors for MetaWriter
    public IReadOnlyList<MI> Order => order;
    public IReadOnlyDictionary<MI, string> Bodies => bodies;
    public HashSet<TT> KlassUsed => klassUsed;
    public HashSet<TT> StaticsUsed => staticsUsed;
    public HashSet<TT> ObjUsed => objUsed;
    public HashSet<TT> Constructed => constructed;
    public IReadOnlyList<string> Strings => strList;
    public IReadOnlyDictionary<MI, int> Gvms => gvms;
    public IReadOnlyList<TT> ConstructedList => constructedList;
    public HashSet<MI> Thunks => thunks;
    public HashSet<MI> Descs => descs;
    public IEnumerable<(FI f, int id)> FieldHandles => fhandles.Values;
    public HashSet<FI> Rvas => rvas;
    public bool IsReached(MI m) => reached.Contains(m);
    public string NameOf(MI m) => Name(m);
}
