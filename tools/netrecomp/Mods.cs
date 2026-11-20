// Mods: opt-in patches applied to the game's IL before it is lifted.
//
// A mod is an assembly (C# compiled against the game, usually through a
// `netrecomp publicize` copy so private members are reachable) whose static
// methods carry an attribute named ReplaceAttribute or PostfixAttribute
// (declared in the mod itself, any namespace) with one string argument,
// "Ns.Type::Method":
//
//   [Replace("Despotism.Foo::Bar")] static int Bar(Foo self, int x)   the body becomes a call to this
//   [Postfix("Despotism.Foo::Awake")] static void Awake(Foo self)      called before every ret (void targets)
//
// Instance targets take `self` first (`ref T` for value types). Overloads are
// told apart by parameter count. See docs/recompiler.md (Mods).
using Mono.Cecil;
using Mono.Cecil.Cil;

static class Mods
{
    public static int Apply(World w, AssemblyDefinition mod)
    {
        int n = 0;
        foreach (var t in mod.MainModule.GetTypes())
            foreach (var m in t.Methods)
                foreach (var ca in m.CustomAttributes)
                {
                    var kind = ca.AttributeType.Name;
                    if (kind is not ("ReplaceAttribute" or "PostfixAttribute") || ca.ConstructorArguments.Count != 1) continue;
                    if (!m.IsStatic) throw new Exception($"mod method {m.FullName} must be static");
                    var target = Find(w, (string)ca.ConstructorArguments[0].Value, m);
                    if (kind == "ReplaceAttribute") Replace(target, m); else Postfix(target, m);
                    Console.WriteLine($"mod: {kind[..^9].ToLowerInvariant()} {target.FullName}");
                    n++;
                }
        return n;
    }

    static MethodDefinition Find(World w, string spec, MethodDefinition hook)
    {
        var sep = spec.IndexOf("::", StringComparison.Ordinal);
        if (sep < 0) throw new Exception($"mod target '{spec}': want Ns.Type::Method");
        var tn = spec[..sep].Replace('+', '/'); var mn = spec[(sep + 2)..];
        foreach (var a in w.Asms.Values)
        {
            var td = a.MainModule.GetType(tn);
            if (td == null) continue;
            var c = td.Methods.Where(x => x.Name == mn && x.Parameters.Count + (x.IsStatic ? 0 : 1) == hook.Parameters.Count).ToList();
            // overloads with the same count: match parameter type names (self excluded)
            if (c.Count > 1)
                c = c.Where(x => x.GenericParameters.Count == hook.GenericParameters.Count && x.Parameters.Select(p => p.ParameterType.Name).SequenceEqual(hook.Parameters.Skip(x.IsStatic ? 0 : 1).Select(p => p.ParameterType.Name))).ToList();
            if (c.Count == 1) return c[0];
            throw new Exception($"mod target '{spec}': {c.Count} methods with {hook.Parameters.Count} parameters (counting self)");
        }
        throw new Exception($"mod target '{spec}': type not found");
    }

    static IEnumerable<Instruction> LoadArgs(MethodDefinition t)
    {
        int n = t.Parameters.Count + (t.IsStatic ? 0 : 1);
        for (int i = 0; i < n; i++) yield return Instruction.Create(OpCodes.Ldarg, Arg(t, i));
    }
    static ParameterDefinition Arg(MethodDefinition t, int i) => t.IsStatic ? t.Parameters[i] : i == 0 ? t.Body.ThisParameter : t.Parameters[i - 1];

    static void Replace(MethodDefinition t, MethodDefinition hook)
    {
        if (!t.HasBody) throw new Exception($"mod: {t.FullName} has no IL body to replace");
        var args = LoadArgs(t).ToList();   // before the body goes: `this` belongs to it
        var b = new MethodBody(t);
        var il = b.GetILProcessor();
        foreach (var a in args) il.Append(a.OpCode == OpCodes.Ldarg && a.Operand == t.Body.ThisParameter ? Instruction.Create(OpCodes.Ldarg_0) : a);
        MethodReference call = t.Module.ImportReference(hook);
        // a generic target calls the hook instantiated at the target's own type parameters
        if (t.HasGenericParameters)
        {
            if (hook.GenericParameters.Count != t.GenericParameters.Count) throw new Exception($"mod: {hook.FullName} needs {t.GenericParameters.Count} type parameters");
            var gi = new GenericInstanceMethod(call);
            foreach (var gp in t.GenericParameters) gi.GenericArguments.Add(gp);
            call = gi;
        }
        il.Append(Instruction.Create(OpCodes.Call, call));
        il.Append(Instruction.Create(OpCodes.Ret));
        t.Body = b;
        Offsets(t);
    }

    // Cecil assigns offsets only when writing; the emitter keys on them.
    static void Offsets(MethodDefinition m)
    {
        int off = 0;
        foreach (var x in m.Body.Instructions) { x.Offset = off; off += x.GetSize(); }
    }

    static void Postfix(MethodDefinition t, MethodDefinition hook)
    {
        if (t.ReturnType.FullName != "System.Void") throw new Exception($"mod: postfix on {t.FullName} needs a void method (use Replace)");
        var il = t.Body.GetILProcessor();
        var call = t.Module.ImportReference(hook);
        foreach (var ret in t.Body.Instructions.Where(x => x.OpCode == OpCodes.Ret).ToList())
        {
            var seq = LoadArgs(t).Append(Instruction.Create(OpCodes.Call, call)).ToList();
            foreach (var x in seq) il.InsertBefore(ret, x);
            // jumps and handler ends that landed on the ret now land on the hook call
            foreach (var x in t.Body.Instructions)
            {
                if (x.Operand == ret && x != seq[^1]) x.Operand = seq[0];
                else if (x.Operand is Instruction[] ts) for (int i = 0; i < ts.Length; i++) if (ts[i] == ret) ts[i] = seq[0];
            }
            foreach (var h in t.Body.ExceptionHandlers)
            {
                if (h.HandlerEnd == ret) h.HandlerEnd = seq[0];
                if (h.TryEnd == ret) h.TryEnd = seq[0];
            }
        }
        Offsets(t);
    }

    // Built-in compat patches for libraries that generate IL at run time, which
    // a static compiler can't: they all have a reflection fallback to switch to.
    public static void Compat(World w)
    {
        void Const(string asm, string type, string method, bool value)
        {
            if (!w.Asms.TryGetValue(asm, out var a)) return;
            var m = a.MainModule.GetType(type)?.Methods.FirstOrDefault(x => x.Name == method && x.HasBody);
            if (m == null) return;
            var b = new MethodBody(m);
            b.GetILProcessor().Append(Instruction.Create(value ? OpCodes.Ldc_I4_1 : OpCodes.Ldc_I4_0));
            b.GetILProcessor().Append(Instruction.Create(OpCodes.Ret));
            m.Body = b;
            Offsets(m);
            Console.WriteLine($"compat: {type}::{method} -> {value}");
        }
        // Json.NET: DynamicMethod-based accessors -> its reflection ones
        Const("Newtonsoft.Json", "Newtonsoft.Json.Serialization.JsonTypeReflector", "get_DynamicCodeGeneration", false);
        // mono 4.x PropertyInfo.GetValue builds a Getter<T,R> delegate with
        // MakeGenericType; call the getter through MethodInfo.Invoke instead
        var mp = w.Corlib.MainModule.GetType("System.Reflection.MonoProperty");
        var gv = mp?.Methods.FirstOrDefault(x => x.Name == "GetValue" && x.Parameters.Count == 2);
        var ggm = mp?.Methods.FirstOrDefault(x => x.Name == "GetGetMethod" && x.Parameters.Count == 1);
        var inv = w.Corlib.MainModule.GetType("System.Reflection.MethodBase")?.Methods.FirstOrDefault(x => x.Name == "Invoke" && x.Parameters.Count == 2);
        if (gv != null && ggm != null && inv != null && gv.Body.Instructions.Any(x => x.Operand is MethodReference r && r.Name == "CreateGetterDelegate"))
        {
            var b = new MethodBody(gv);
            var il = b.GetILProcessor();
            il.Append(Instruction.Create(OpCodes.Ldarg_0));
            il.Append(Instruction.Create(OpCodes.Ldc_I4_1));
            il.Append(Instruction.Create(OpCodes.Callvirt, ggm));
            il.Append(Instruction.Create(OpCodes.Ldarg_1));
            il.Append(Instruction.Create(OpCodes.Ldarg_2));
            il.Append(Instruction.Create(OpCodes.Callvirt, inv));
            il.Append(Instruction.Create(OpCodes.Ret));
            gv.Body = b;
            Offsets(gv);
            Console.WriteLine("compat: MonoProperty::GetValue -> GetGetMethod(true).Invoke");
        }
    }

    // A copy of an assembly with every type and member public, so mod C#
    // can be compiled against the game's private fields and methods.
    public static int Publicize(string src, string dst)
    {
        var res = new DefaultAssemblyResolver();
        res.AddSearchDirectory(Path.GetDirectoryName(Path.GetFullPath(src))!);
        var a = AssemblyDefinition.ReadAssembly(src, new ReaderParameters { AssemblyResolver = res });
        foreach (var t in a.MainModule.GetTypes())
        {
            if (t.IsNested) t.IsNestedPublic = true; else t.IsPublic = true;
            foreach (var m in t.Methods) m.IsPublic = true;
            // a field named like an event clashes with the event once public
            var events = new HashSet<string>(t.Events.Select(e => e.Name));
            foreach (var f in t.Fields) if (!events.Contains(f.Name)) f.IsPublic = true;
        }
        a.Write(dst);
        return 0;
    }
}
