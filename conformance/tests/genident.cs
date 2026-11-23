// One class per closed generic: a shared generic method run through MakeGenericMethod
// returns the List<T> the caller asked for, which FieldInfo.SetValue accepts.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;

class Mod { public string Name; public override string ToString() { return Name; } }
class Holder { public List<Mod> Mods; }

static class P
{
    static int Main()
    {
        var h = new Holder();
        Mod[] arr = { new Mod { Name = "a" }, new Mod { Name = "b" } };
        MethodInfo m = typeof(Enumerable).GetMethod("ToList").MakeGenericMethod(typeof(Mod));
        object r = m.Invoke(null, new object[] { arr });
        Console.WriteLine(r.GetType() == typeof(List<Mod>));
        typeof(Holder).GetField("Mods").SetValue(h, r);
        Console.WriteLine(h.Mods.Count + " " + h.Mods[1]);
        Console.WriteLine(r is List<Mod>);
        return 0;
    }
}
