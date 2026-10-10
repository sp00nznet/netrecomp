// Non-generic hashed collections with keys that override GetHashCode and
// Equals (Unity objects hash by instance id): Hashtable, OrderedDictionary
// (NGUI's panel child list), and Dictionary for comparison.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.Specialized;

class Key
{
    public int id;
    public Key(int id) { this.id = id; }
    public override int GetHashCode() { return id; }
    public override bool Equals(object o) { var k = o as Key; return k != null && k.id == id; }
}

class Node { public Key k; public Node(Key k) { this.k = k; } }

static class Program
{
    static int Main()
    {
        var keys = new List<Key>();
        for (int i = 0; i < 40; i++) keys.Add(new Key(1000 + i * 7));
        var od = new OrderedDictionary();
        foreach (var k in keys) if (!od.Contains(k)) od.Add(k, new Node(k));
        Console.WriteLine(od.Count + " " + od.Contains(new Key(1007)) + " " + od.Contains(new Key(5)));
        Console.WriteLine(((Node)od[3]).k.id + " " + ((Node)od[(object)new Key(1014)]).k.id);
        od.Remove(keys[0]);
        Console.WriteLine(od.Count + " " + ((Node)od[0]).k.id);
        var ht = new Hashtable();
        foreach (var k in keys) ht[k] = k.id;
        ht[new Key(1000)] = -1;
        Console.WriteLine(ht.Count + " " + ht[new Key(1000)] + " " + ht.ContainsKey(new Key(1273)));
        var d = new Dictionary<object, int>();
        foreach (var k in keys) d[k] = k.id;
        Console.WriteLine(d.Count + " " + d[new Key(1021)]);
        object o = new Key(3);
        Console.WriteLine(o.GetHashCode() + " " + o.Equals(new Key(3)) + " " + "abc".GetHashCode().Equals("abc".GetHashCode()));
        return 0;
    }
}
