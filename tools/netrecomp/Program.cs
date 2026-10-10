// netrecomp: .NET IL -> C static recompiler. See docs/architecture.md.
using Mono.Cecil;
using Mono.Cecil.Cil;

static class Program
{
    static int Main(string[] args)
    {
        if (args.Length >= 2 && args[0] == "stats") return Stats(args[1]);
        if (args.Length >= 3 && args[0] == "dump") return Dump(args[1], args.Skip(2).ToArray());
        if (args.Length >= 2 && args[0] == "lift") return Lift(args.Skip(1).ToArray());
        Console.Error.WriteLine("usage: netrecomp lift <Managed dir> -o <gen dir> [--exe <assembly>] [--report <file>]\n" +
                                "       netrecomp stats <Managed dir>\n       netrecomp dump <dll> <Type>...");
        return 2;
    }

    // Framework assemblies are compiled by reachability only; everything else
    // in the folder is the game (or its engine glue) and is compiled whole.
    static readonly HashSet<string> Framework = new(StringComparer.OrdinalIgnoreCase)
    {
        "mscorlib", "System", "System.Core", "System.Xml", "Mono.Security", "I18N", "I18N.CJK", "I18N.MidEast",
        "I18N.Other", "I18N.West", "I18N.Rare", "Boo.Lang", "UnityScript.Lang", "System.Configuration", "System.Security",
    };

    // Types the runtime itself constructs or throws.
    static readonly string[] RuntimeTypes =
    {
        "System.Object", "System.String", "System.MonoType", "System.Exception", "System.SystemException",
        "System.NullReferenceException", "System.IndexOutOfRangeException", "System.InvalidCastException",
        "System.OverflowException", "System.DivideByZeroException", "System.ArithmeticException",
        "System.OutOfMemoryException", "System.StackOverflowException", "System.ArrayTypeMismatchException",
        "System.NotSupportedException", "System.NotImplementedException", "System.MissingMethodException",
        "System.MissingFieldException", "System.TypeInitializationException", "System.ArgumentException",
        "System.ArgumentNullException", "System.ArgumentOutOfRangeException", "System.ExecutionEngineException",
        "System.InvalidOperationException", "System.TypeLoadException", "System.Threading.ThreadAbortException",
        "System.Threading.Thread", "System.Reflection.MonoMethod", "System.Reflection.MonoCMethod",
        "System.Reflection.MonoField", "System.Reflection.MonoProperty", "System.Reflection.MonoEvent",
        "System.Reflection.Assembly", "System.Reflection.Module", "System.Reflection.ParameterInfo",
        "System.Reflection.TargetInvocationException", "System.IO.IOException", "System.IO.FileNotFoundException",
        "System.IO.DirectoryNotFoundException", "System.IO.EndOfStreamException", "System.UnauthorizedAccessException",
        "System.Security.SecurityException", "System.FormatException", "System.Collections.Generic.KeyNotFoundException",
        "System.AppDomain", "System.Int32", "System.Boolean", "System.Byte", "System.Char", "System.Int64",
        "System.Single", "System.Double", "System.IntPtr", "System.Int16", "System.UInt16", "System.UInt32",
        "System.UInt64", "System.SByte", "System.UIntPtr", "System.Void", "System.DBNull", "System.Reflection.Missing",
    };

    static int Lift(string[] args)
    {
        string dir = args[0], outDir = "gen";
        string? exe = null, report = null;
        for (int i = 1; i < args.Length; i++)
            switch (args[i])
            {
                case "-o": outDir = args[++i]; break;
                case "--exe": exe = args[++i]; break;
                case "--report": report = args[++i]; break;
                default: Console.Error.WriteLine("unknown option " + args[i]); return 2;
            }
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var w = new World(dir);
        if (exe != null)
        {
            var a = AssemblyDefinition.ReadAssembly(exe, new ReaderParameters { AssemblyResolver = w.Resolver });
            w.Asms[a.Name.Name] = a;
            w.Resolver.Add(a);
        }
        var g = new Gen(w);
        var none = Array.Empty<TT>();
        foreach (var n in RuntimeTypes)
        {
            var dot = n.LastIndexOf('.');
            var t = w.Cor(n[..dot], n[(dot + 1)..]);
            g.Construct(t);
            foreach (var c in t.Def.Methods.Where(m => m.IsConstructor && !m.IsStatic && m.Parameters.Count <= 1 && !m.HasGenericParameters))
                g.Root(w.Method(t, c, none));
        }
        foreach (var et in new[] { "Object", "Byte", "Char", "String", "Int32", "Single" })
            g.Construct(w.SzArray(w.Cor("System", et)));

        if (exe != null)
        {
            var a = w.Asms.Values.First(x => x.MainModule.FileName == Path.GetFullPath(exe) || x.MainModule.EntryPoint != null && x.Name.Name == Path.GetFileNameWithoutExtension(exe));
            var ep = a.MainModule.EntryPoint ?? throw new Exception("no entry point in " + exe);
            g.Entry = w.Method(w.Inst(ep.DeclaringType, none), ep, none);
            g.Root(g.Entry);
            RootWhole(g, w, a);
        }
        else
        {
            foreach (var a in w.Asms.Values.Where(a => !Framework.Contains(a.Name.Name)).OrderBy(a => a.Name.Name))
                RootWhole(g, w, a);
        }
        g.Run();
        g.Write(outDir);
        int reached = g.Ok + g.FailedMethods.Count;
        Console.WriteLine($"lifted {g.Ok}/{reached} methods ({g.Icalls} icalls, {g.Pinvokes - g.PinvokeFail}/{g.Pinvokes} pinvokes) in {sw.Elapsed.TotalSeconds:f1}s -> {outDir}");
        foreach (var kv in g.Failures.OrderByDescending(k => k.Value).Take(15)) Console.WriteLine($"  {kv.Value,6}  {kv.Key}");
        if (report != null) File.WriteAllLines(report, g.FailedMethods);
        return 0;
    }

    // Game code is compiled whole: the engine (or reflection) reaches it by name.
    static void RootWhole(Gen g, World w, AssemblyDefinition a)
    {
        var none = Array.Empty<TT>();
        foreach (var t in a.MainModule.GetTypes())
        {
            if (t.HasGenericParameters || IsOpenNested(t)) continue;
            var tt = w.Inst(t, none);
            if (!t.IsInterface && !t.IsAbstract) g.Construct(tt);
            foreach (var m in t.Methods)
                if (!m.HasGenericParameters && !m.IsAbstract) g.Root(w.Method(tt, m, none));
        }
    }

    static bool IsOpenNested(TypeDefinition t)
    {
        for (var d = t.DeclaringType; d != null; d = d.DeclaringType) if (d.HasGenericParameters) return true;
        return false;
    }

    // Opcode histogram and feature counts over every assembly in a folder.
    static int Stats(string dir)
    {
        var ops = new Dictionary<string, int>();
        int types = 0, methods = 0, bodies = 0, icalls = 0, pinv = 0, generic = 0, filters = 0, explicitLayout = 0;
        long ilBytes = 0;
        foreach (var f in Directory.GetFiles(dir, "*.dll").OrderBy(x => x))
        {
            var m = ModuleDefinition.ReadModule(f);
            int mb = 0;
            foreach (var t in m.GetTypes())
            {
                types++;
                if (t.IsExplicitLayout) explicitLayout++;
                foreach (var md in t.Methods)
                {
                    methods++;
                    if (md.HasGenericParameters || t.HasGenericParameters) generic++;
                    if (md.IsInternalCall) icalls++;
                    if (md.IsPInvokeImpl) pinv++;
                    if (!md.HasBody) continue;
                    bodies++; mb++;
                    ilBytes += md.Body.CodeSize;
                    foreach (var h in md.Body.ExceptionHandlers) if (h.HandlerType == ExceptionHandlerType.Filter) filters++;
                    foreach (var i in md.Body.Instructions)
                        ops[i.OpCode.Name] = ops.GetValueOrDefault(i.OpCode.Name) + 1;
                }
            }
            Console.WriteLine($"{Path.GetFileName(f),-36} {mb,6} bodies");
        }
        Console.WriteLine($"types {types} methods {methods} bodies {bodies} il {ilBytes} generic {generic} icalls {icalls} pinvoke {pinv} filters {filters} explicit {explicitLayout}");
        foreach (var kv in ops.OrderByDescending(k => k.Value)) Console.Write($"{kv.Key}={kv.Value} ");
        Console.WriteLine();
        return 0;
    }

    // Fields and method signatures of named types: dump <dll> <Ns.Type>...
    static int Dump(string dll, string[] names)
    {
        var m = ModuleDefinition.ReadModule(dll);
        foreach (var n in names)
        {
            var t = m.GetType(n);
            if (t == null) { Console.WriteLine("no " + n); continue; }
            Console.WriteLine($"{t.FullName} : {t.BaseType?.FullName} {t.Attributes}");
            foreach (var f in t.Fields) Console.WriteLine($"  F {(f.IsStatic ? "static " : "")}{f.FieldType.FullName} {f.Name}");
            foreach (var md in t.Methods) Console.WriteLine($"  M {md.ImplAttributes} {(md.IsStatic ? "static " : "")}{md.FullName}");
        }
        return 0;
    }
}
