// Generic collections and LINQ (System.Core), string switch, sorting with
// comparers, nested generics, struct keys.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Text;

struct Key : IEquatable<Key>
{
    public int A; public string B;
    public Key(int a, string b) { A = a; B = b; }
    public bool Equals(Key o) { return A == o.A && B == o.B; }
    public override bool Equals(object o) { return o is Key && Equals((Key)o); }
    public override int GetHashCode() { return A * 31 + (B == null ? 0 : B.GetHashCode()); }
}

class Item { public string Name; public int Cost; public Item(string n, int c) { Name = n; Cost = c; } }

static class Program
{
    static string Kind(string s)
    {
        switch (s)
        {
            case "apple": case "pear": return "fruit";
            case "carrot": return "vegetable";
            case "steak": case "chicken": case "fish": case "pork": case "lamb": case "duck": return "meat";
            default: return "unknown";
        }
    }

    static int Main()
    {
        var list = new List<int>();
        for (int i = 0; i < 20; i++) list.Add((i * 7) % 13);
        list.Sort();
        Console.WriteLine(string.Join(",", list.Select(x => x.ToString()).ToArray()));
        list.Sort((a, b) => b.CompareTo(a));
        Console.WriteLine(list[0] + " " + list[list.Count - 1] + " " + list.IndexOf(5) + " " + list.Contains(12));
        list.RemoveAll(x => x % 2 == 0);
        Console.WriteLine(list.Count);

        var evens = Enumerable.Range(1, 30).Where(x => x % 3 == 0).Select(x => x * x).ToList();
        Console.WriteLine(evens.Sum() + " " + evens.Max() + " " + evens.Min() + " " + evens.Average());
        var groups = new[] { "ant", "bee", "cat", "cow", "dog", "deer", "eel" }.GroupBy(w => w[0]).OrderByDescending(g => g.Count()).ThenBy(g => g.Key);
        foreach (var g in groups) Console.Write(g.Key + ":" + g.Count() + " ");
        Console.WriteLine();
        var items = new List<Item> { new Item("sword", 300), new Item("medkit", 50), new Item("armor", 450), new Item("grenade", 75) };
        foreach (var it in items.OrderBy(i => i.Cost).Take(3)) Console.Write(it.Name + " ");
        Console.WriteLine();
        Console.WriteLine(items.Any(i => i.Cost > 400) + " " + items.All(i => i.Cost > 10) + " " + items.First(i => i.Name.StartsWith("m")).Cost);
        var dict = items.ToDictionary(i => i.Name, i => i.Cost);
        Console.WriteLine(dict["armor"] + dict.Count);

        var sk = new Dictionary<Key, string>();
        sk[new Key(1, "x")] = "one-x"; sk[new Key(2, "y")] = "two-y"; sk[new Key(1, "x")] = "again";
        Console.WriteLine(sk.Count + " " + sk[new Key(1, "x")] + " " + sk.ContainsKey(new Key(2, "z")));

        var hs = new HashSet<string>(new[] { "a", "b", "a", "c" });
        Console.WriteLine(hs.Count + " " + hs.Contains("b"));
        var q = new Queue<int>(); q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); q.Dequeue();
        var st = new Stack<string>(); st.Push("x"); st.Push("y");
        Console.WriteLine(q.Peek() + st.Pop() + st.Count);
        var sorted = new SortedDictionary<string, int> { { "z", 1 }, { "a", 2 }, { "m", 3 } };
        foreach (var kv in sorted) Console.Write(kv.Key + kv.Value);
        Console.WriteLine();
        var ll = new LinkedList<int>(); ll.AddLast(1); ll.AddFirst(0); ll.AddLast(2);
        foreach (var v in ll) Console.Write(v);
        Console.WriteLine();

        var nested = new Dictionary<string, List<KeyValuePair<int, string>>>();
        nested["k"] = new List<KeyValuePair<int, string>> { new KeyValuePair<int, string>(1, "a") };
        nested["k"].Add(new KeyValuePair<int, string>(2, "b"));
        Console.WriteLine(nested["k"].Count + nested["k"][1].Value);

        var al = new ArrayList { 1, "two", 3.0 };
        foreach (var o in al) Console.Write(o.GetType().Name + " ");
        Console.WriteLine();
        var ht = new Hashtable(); ht["a"] = 1; ht[2] = "b";
        Console.WriteLine(ht["a"] + "" + ht[2]);

        foreach (var s in new[] { "apple", "carrot", "lamb", "rock" }) Console.Write(Kind(s) + " ");
        Console.WriteLine();
        int[] arr = { 4, 2, 7 };
        Array.Reverse(arr); Console.WriteLine(arr[0] + "" + arr[2] + Array.IndexOf(arr, 2));
        var copy = new int[5]; Array.Copy(arr, 0, copy, 1, 3); Console.WriteLine(string.Join("", copy.Select(x => x.ToString()).ToArray()));
        object[] objs = new string[] { "s" };
        try { objs[0] = 1; } catch (ArrayTypeMismatchException) { Console.WriteLine("covariance checked"); }
        var nums = new List<double> { 1.5, 2.25, -3 };
        Console.WriteLine(nums.Aggregate((a, b) => a * b));
        Console.WriteLine(Enumerable.Repeat("ab", 3).Aggregate(new StringBuilder(), (sb, s) => sb.Append(s)).ToString());
        return 0;
    }
}
