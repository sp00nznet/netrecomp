// Concrete (fully instantiated) types and methods over Cecil definitions.
// Every generic is expanded per instantiation: no shared code, no runtime
// generic context. See docs/recompiler.md "Generics".
using Mono.Cecil;

enum TK { Def, SzArray, Array, Ptr, ByRef, FnPtr }

sealed class TT
{
    public TK K;
    public TypeDefinition Def = null!;   // for Def
    public TT[] Args = Array.Empty<TT>();
    public TT Elem = null!;              // arrays, ptr, byref
    public int Rank;
    public string Key = "";
    public int Id = -1;                  // assigned when the type is used by generated code
    public override string ToString() => Key;

    public bool IsValueType => K == TK.Def && Def.IsValueType;
    public bool IsEnum => K == TK.Def && Def.IsEnum;
    public bool IsInterface => K == TK.Def && Def.IsInterface;
    public bool IsRef => K == TK.SzArray || K == TK.Array || (K == TK.Def && !Def.IsValueType);
    public bool IsGenericInst => Args.Length > 0;
    // typeof(List<>): a generic definition used as a type. Metadata only.
    public bool IsOpen => K == TK.Def && Def.HasGenericParameters && Args.Length == 0;
    public string Ns => K == TK.Def ? (Def.DeclaringType != null ? "" : Def.Namespace) : "";

    // Layout and dispatch, filled by Layout.cs
    public Layout? L;
}

sealed class MI
{
    public TT Owner = null!;
    public MethodDefinition Def = null!;
    public TT[] MArgs = Array.Empty<TT>();
    public string Key = "";
    public int Id = -1;
    public string CName = "";
    public override string ToString() => Key;
}

sealed class FI
{
    public TT Owner = null!;
    public FieldDefinition Def = null!;
    public TT Type = null!;
}

sealed class World
{
    public readonly Resolver Resolver = new();
    public readonly Dictionary<string, AssemblyDefinition> Asms = new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<string, TT> types = new();
    readonly Dictionary<string, MI> methods = new();
    public AssemblyDefinition Corlib = null!;

    public World(string dir)
    {
        Resolver.AddSearchDirectory(dir);
        foreach (var d in Resolver.GetSearchDirectories().ToArray())
            if (d != dir) Resolver.RemoveSearchDirectory(d);
        var rp = new ReaderParameters { AssemblyResolver = Resolver, ReadingMode = ReadingMode.Deferred };
        foreach (var f in Directory.GetFiles(dir, "*.dll").OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
        {
            AssemblyDefinition a;
            try { a = AssemblyDefinition.ReadAssembly(f, rp); } catch (BadImageFormatException) { continue; }
            Asms[a.Name.Name] = a;
            Resolver.Add(a);
        }
        Corlib = Asms["mscorlib"];
    }

    public TypeDefinition CorDef(string ns, string name) =>
        Corlib.MainModule.GetType(ns, name) ?? throw new Exception($"corlib lacks {ns}.{name}");
    public TT Cor(string ns, string name) => Inst(CorDef(ns, name), Array.Empty<TT>());

    static string Join(TT[] a) => a.Length == 0 ? "" : "<" + string.Join(",", a.Select(x => x.Key)) + ">";

    public TT Inst(TypeDefinition d, TT[] args)
    {
        var key = "[" + d.Module.Assembly.Name.Name + "]" + d.FullName + Join(args);
        if (types.TryGetValue(key, out var t)) return t;
        t = new TT { K = TK.Def, Def = d, Args = args, Key = key };
        types[key] = t;
        return t;
    }

    TT Wrap(TK k, TT e, int rank, string suffix)
    {
        var key = e.Key + suffix;
        if (types.TryGetValue(key, out var t)) return t;
        t = new TT { K = k, Elem = e, Rank = rank, Key = key };
        types[key] = t;
        return t;
    }
    public TT SzArray(TT e) => Wrap(TK.SzArray, e, 1, "[]");
    public TT MdArray(TT e, int rank) => Wrap(TK.Array, e, rank, "[" + new string(',', rank - 1) + "*]");
    public TT Ptr(TT e) => Wrap(TK.Ptr, e, 0, "*");
    public TT ByRef(TT e) => Wrap(TK.ByRef, e, 0, "&");
    public TT FnPtr => Wrap(TK.FnPtr, Cor("System", "IntPtr"), 0, "#fnptr");

    public IEnumerable<TT> AllTypes => types.Values;

    public TT Conv(TypeReference r, TT[] ta, TT[] ma)
    {
        switch (r)
        {
            case GenericParameter gp:
                var src = gp.Type == GenericParameterType.Type ? ta : ma;
                if (gp.Position >= src.Length) throw new UnresolvedGeneric(gp.FullName);
                return src[gp.Position];
            case ArrayType a:
                return a.IsVector ? SzArray(Conv(a.ElementType, ta, ma)) : MdArray(Conv(a.ElementType, ta, ma), a.Rank);
            case ByReferenceType b: return ByRef(Conv(b.ElementType, ta, ma));
            case PointerType p: return Ptr(Conv(p.ElementType, ta, ma));
            case PinnedType p: return Conv(p.ElementType, ta, ma);
            case RequiredModifierType m: return Conv(m.ElementType, ta, ma);
            case OptionalModifierType m: return Conv(m.ElementType, ta, ma);
            case SentinelType s: return Conv(s.ElementType, ta, ma);
            case FunctionPointerType: return FnPtr;
            case GenericInstanceType g:
                return Inst(Resolve(g.ElementType), g.GenericArguments.Select(x => Conv(x, ta, ma)).ToArray());
            default:
                return Inst(Resolve(r), Array.Empty<TT>());
        }
    }

    // Falls back to a by-name search: a program built against .NET 4 refers to
    // [mscorlib]System.Func`2, which the 2.0 profile keeps in System.Core.
    public TypeDefinition Resolve(TypeReference r)
    {
        TypeDefinition? d = null;
        try { d = r.Resolve(); } catch (AssemblyResolutionException) { }
        return d ?? ByName(r) ?? throw new EmitError("cannot resolve type " + r.FullName);
    }

    TypeDefinition? ByName(TypeReference r)
    {
        if (r.IsNested)
            return ByName(r.DeclaringType)?.NestedTypes.FirstOrDefault(n => n.Name == r.Name);
        foreach (var a in Asms.Values)
            if (a.MainModule.GetType(r.Namespace, r.Name) is TypeDefinition t) return t;
        return null;
    }

    public MethodDefinition ResolveMethod(MethodReference mr)
    {
        MethodDefinition? d = null;
        try { d = mr.Resolve(); } catch (AssemblyResolutionException) { }
        if (d != null) return d;
        var owner = Resolve(mr.DeclaringType.GetElementType());
        static string Sig(TypeReference t) => t switch
        {
            GenericParameter g => (g.Type == GenericParameterType.Type ? "!" : "!!") + g.Position,
            ByReferenceType b => Sig(b.ElementType) + "&",
            ArrayType a => Sig(a.ElementType) + "[]",
            PointerType p => Sig(p.ElementType) + "*",
            GenericInstanceType gi => Sig(gi.ElementType) + "<" + string.Join(",", gi.GenericArguments.Select(Sig)) + ">",
            _ => t.Name,
        };
        d = owner.Methods.FirstOrDefault(m => m.Name == mr.Name && m.Parameters.Count == mr.Parameters.Count
            && m.GenericParameters.Count == mr.GenericParameters.Count && m.HasThis == mr.HasThis
            && m.Parameters.Select(p => Sig(p.ParameterType)).SequenceEqual(mr.Parameters.Select(p => Sig(p.ParameterType))));
        return d ?? throw new EmitError("cannot resolve method " + mr.FullName);
    }

    public FieldDefinition ResolveField(FieldReference fr)
    {
        FieldDefinition? d = null;
        try { d = fr.Resolve(); } catch (AssemblyResolutionException) { }
        return d ?? Resolve(fr.DeclaringType.GetElementType()).Fields.FirstOrDefault(f => f.Name == fr.Name)
            ?? throw new EmitError("cannot resolve field " + fr.FullName);
    }

    public MI Method(TT owner, MethodDefinition d, TT[] margs)
    {
        var key = owner.Key + "::" + d.Name + Join(margs) + "(" + string.Join(",", d.Parameters.Select(p => p.ParameterType.FullName)) + ")" + d.ReturnType.FullName + (d.IsStatic ? "s" : "");
        if (methods.TryGetValue(key, out var m)) return m;
        m = new MI { Owner = owner, Def = d, MArgs = margs, Key = key };
        methods[key] = m;
        return m;
    }

    // A call-site reference to a concrete method, in the caller's generic context.
    public MI? ConvMethod(MethodReference mr, TT[] ta, TT[] ma)
    {
        var owner = Conv(mr.DeclaringType, ta, ma);
        var margs = mr is GenericInstanceMethod gim ? gim.GenericArguments.Select(x => Conv(x, ta, ma)).ToArray() : Array.Empty<TT>();
        if (owner.K != TK.Def) return null;   // multi-dim array runtime methods
        var d = ResolveMethod(mr);
        return Method(owner, d, margs);
    }

    public FI Field(FieldReference fr, TT[] ta, TT[] ma)
    {
        var owner = Conv(fr.DeclaringType, ta, ma);
        var d = ResolveField(fr);
        return new FI { Owner = owner, Def = d, Type = Conv(d.FieldType, owner.Args, Array.Empty<TT>()) };
    }

    public TT RetType(MI m) => Conv(m.Def.ReturnType, m.Owner.Args, m.MArgs);
    public TT ParamType(MI m, int i) => Conv(m.Def.Parameters[i].ParameterType, m.Owner.Args, m.MArgs);
    public TT FieldType(TT owner, FieldDefinition f) => Conv(f.FieldType, owner.Args, Array.Empty<TT>());
    public TT? BaseOf(TT t) => t.K == TK.Def ? (t.Def.BaseType == null || t.IsOpen ? null : Conv(t.Def.BaseType, t.Args, Array.Empty<TT>()))
                              : t.K is TK.SzArray or TK.Array ? Cor("System", "Array") : null;
}

// Resolution must hand back the definitions we loaded, not fresh copies:
// layouts and slots are keyed by definition identity.
// By simple name: compact-framework assemblies reference mscorlib 3.5.
sealed class Resolver : DefaultAssemblyResolver
{
    readonly Dictionary<string, AssemblyDefinition> byName = new(StringComparer.OrdinalIgnoreCase);
    public void Add(AssemblyDefinition a) { byName[a.Name.Name] = a; RegisterAssembly(a); }
    public override AssemblyDefinition Resolve(AssemblyNameReference name, ReaderParameters p) =>
        byName.TryGetValue(name.Name, out var a) ? a : base.Resolve(name, p);
}

sealed class UnresolvedGeneric(string what) : Exception("open generic parameter " + what);
