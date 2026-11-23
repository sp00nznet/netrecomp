// MakeGenericMethod over runtime types: canonical instances, interface generic virtuals.
using System;
using System.Reflection;

interface ISer { string Ser<TIn, TOut>(TIn v); }
class JSer : ISer { public string Ser<TIn, TOut>(TIn v) { return "j:" + v; } }
class Box { public int N; public override string ToString() { return "box" + N; } }
static class Util { public static string Show<T>(T v) where T : class { return "[" + v + "]"; } }

static class P
{
    static int Main()
    {
        ISer s = new JSer();
        object body = new Box { N = 7 };
        MethodInfo m = typeof(ISer).GetMethod("Ser");
        Console.WriteLine(m.IsGenericMethodDefinition);
        MethodInfo g = m.MakeGenericMethod(body.GetType(), typeof(string));
        Console.WriteLine(g.IsGenericMethod + " " + g.IsGenericMethodDefinition);
        Console.WriteLine(g.Invoke(s, new object[] { body }));
        MethodInfo u = typeof(Util).GetMethod("Show").MakeGenericMethod(typeof(Box));
        Console.WriteLine(u.Invoke(null, new object[] { body }));
        var ls = new System.Collections.Generic.List<string>();
        Console.WriteLine(Type.GetType("System.Collections.Generic.List`1[[System.String, mscorlib, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089]]") == ls.GetType());
        Console.WriteLine(Type.GetType("System.Collections.Generic.List`1[System.String][]") == typeof(System.Collections.Generic.List<string>[]));
        Console.WriteLine(ls.GetType().GetGenericTypeDefinition() == typeof(System.Collections.Generic.List<>));
        // array types have runtime constructors (a serializer's Activator.CreateInstance(T[], n))
        Box[] arr = (Box[])Activator.CreateInstance(typeof(Box[]), 3);
        int[,] md = (int[,])Activator.CreateInstance(typeof(int[,]), 2, 3);
        Console.WriteLine(arr.Length + " " + md.Length + " " + md.GetLength(1));
        return 3;
    }
}
