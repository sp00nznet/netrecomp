// The same null dereference, caught again and again with collections in between:
// every catch sees a live NullReferenceException of its own.
using System;

class Node { public int V; }

static class P
{
    static int Hit(Node n) { return n.V; }

    static int Main()
    {
        int ok = 0;
        Exception prev = null;
        for (int i = 0; i < 2000; i++)
        {
            try { Hit(null); }
            catch (NullReferenceException e)
            {
                if (e != prev && e.GetType() == typeof(NullReferenceException)) ok++;
                prev = e;
            }
            if (i % 100 == 0) { GC.Collect(); var junk = new object[1000]; for (int j = 0; j < junk.Length; j++) junk[j] = new byte[64]; }
        }
        Console.WriteLine(ok);
        return 0;
    }
}
