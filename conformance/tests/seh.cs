// Exception regions in the shapes generated code leans on: foreach over a
// struct enumerator (try/finally with a boxed Dispose) whose body continues,
// breaks and calls something that throws and catches internally; nested
// finally; return from inside try; exceptions crossing finally blocks.
using System;
using System.Collections.Generic;

static class Program
{
    static int loads;
    static object Load(string key)
    {
        try
        {
            loads++;
            if (key.Length % 2 == 0) throw new System.IO.FileNotFoundException(key);
            return key.ToUpper();
        }
        catch (System.IO.FileNotFoundException) { return null; }
    }

    static string Atlas(Dictionary<string, int> names)
    {
        var list = new List<object>();
        var seen = new List<string>();
        int num = 0;
        foreach (string key in names.Keys)
        {
            if (seen.Contains(key)) continue;
            object tex = Load(key);
            if (tex == null) continue;
            list.Add(tex);
            seen.Add(key);
            num++;
        }
        names.Clear();
        int i = 0;
        foreach (string item in seen)
        {
            names.Add(item, i);
            i++;
        }
        return string.Format("{0} of {1}: {2}", num, loads, string.Join(",", list.ConvertAll(o => o.ToString()).ToArray()));
    }

    static int Early(List<int> xs)
    {
        foreach (int x in xs)
        {
            try { if (x == 3) return x * 10; }
            finally { loads += 100; }
        }
        return -1;
    }

    static int Crossing()
    {
        int r = 0;
        try
        {
            try { try { throw new InvalidOperationException("a"); } finally { r += 1; } }
            finally { r += 10; }
        }
        catch (InvalidOperationException) { r += 100; }
        foreach (var kv in new Dictionary<int, int> { { 1, 2 } })
        {
            try { throw new ArgumentException(); }
            catch (ArgumentException) { r += kv.Value * 1000; }
        }
        return r;
    }

    static int Main()
    {
        var names = new Dictionary<string, int>();
        foreach (var n in new[] { "abc", "de", "fgh", "ijkl", "m", "abc2" }) names[n] = 0;
        for (int k = 0; k < 3; k++)
        {
            Console.WriteLine(Atlas(names));
            foreach (var n in new[] { "zz", "yyy", "x" }) names[n + k] = 0;
        }
        Console.WriteLine(Early(new List<int> { 1, 2, 3, 4 }) + " " + loads);
        Console.WriteLine(Crossing());
        return 0;
    }
}
