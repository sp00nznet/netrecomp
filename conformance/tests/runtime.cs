// Parsing and formatting, reflection, enums, threads and locks, file I/O,
// static init order, nullable, struct math, finally ordering.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Threading;

[Flags] enum Perm { None = 0, Read = 1, Write = 2, Exec = 4 }
enum Level : byte { Low = 1, Mid, High }

struct Vec3
{
    public float x, y, z;
    public Vec3(float x, float y, float z) { this.x = x; this.y = y; this.z = z; }
    public static Vec3 operator +(Vec3 a, Vec3 b) { return new Vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
    public static Vec3 operator *(Vec3 a, float s) { return new Vec3(a.x * s, a.y * s, a.z * s); }
    public float Dot(Vec3 o) { return x * o.x + y * o.y + z * o.z; }
    public override string ToString() { return string.Format("({0:F1}, {1:F1}, {2:F1})", x, y, z); }
}

class Widget
{
    public int Size = 3;
    public string Label { get; set; }
    public Widget() { Label = "w"; }
    public int Grow(int by) { Size += by; return Size; }
    static int hidden = 42;
}

class Base { public virtual string Who() { return "base"; } }
class Derived : Base { public override string Who() { return "derived"; } }

static class Program
{
    static int counter;
    static readonly object gate = new object();

    static void Work()
    {
        for (int i = 0; i < 10000; i++) lock (gate) counter++;
    }

    static string Order()
    {
        var log = new List<string>();
        try
        {
            try { log.Add("a"); throw new Exception("x"); }
            finally { log.Add("b"); }
        }
        catch (Exception e) { log.Add("c" + e.Message); }
        finally { log.Add("d"); }
        return string.Join("", log.ToArray());
    }

    static int Main()
    {
        var inv = CultureInfo.InvariantCulture;
        Console.WriteLine(int.Parse("-1234") + 1);
        Console.WriteLine(double.Parse("3.25", inv) * 2);
        Console.WriteLine(float.Parse("1e-3", inv));
        int r; Console.WriteLine(int.TryParse("12x", out r) + " " + r);
        Console.WriteLine(long.MaxValue.ToString("N0", inv));
        Console.WriteLine((0.1 + 0.2).ToString(inv));
        Console.WriteLine(255.ToString("x") + " " + 3.14159.ToString("0.00", inv) + " " + 0.5.ToString("P0", inv));
        Console.WriteLine(new DateTime(2013, 7, 25, 10, 30, 0).ToString("yyyy-MM-dd HH:mm", inv));
        Console.WriteLine(TimeSpan.FromSeconds(3725).ToString());
        Console.WriteLine(Convert.ToBase64String(new byte[] { 1, 2, 3, 250 }));
        Console.WriteLine(BitConverter.ToString(BitConverter.GetBytes(0x01020304)));
        Console.WriteLine(Guid.Empty.ToString());

        Perm p = Perm.Read | Perm.Exec;
        Console.WriteLine(p + " " + (p & Perm.Write) + " " + (int)p);
        Console.WriteLine((Level)Enum.Parse(typeof(Level), "Mid") + " " + Enum.GetNames(typeof(Level)).Length + " " + Level.High.CompareTo(Level.Low));
        Console.WriteLine(Enum.IsDefined(typeof(Level), (byte)3));

        var w = new Widget();
        Type t = w.GetType();
        Console.WriteLine(t.Name + " " + t.FullName + " " + typeof(List<int>).Name);
        Console.WriteLine(t.GetField("Size").GetValue(w));
        t.GetField("Size").SetValue(w, 10);
        Console.WriteLine(t.GetMethod("Grow").Invoke(w, new object[] { 5 }));
        Console.WriteLine(t.GetProperty("Label").GetValue(w, null));
        var w2 = (Widget)Activator.CreateInstance(typeof(Widget));
        Console.WriteLine(w2.Size + w2.Label);
        Console.WriteLine(t.GetField("hidden", BindingFlags.NonPublic | BindingFlags.Static).GetValue(null));
        Base b = new Derived();
        Console.WriteLine(typeof(Base).GetMethod("Who").Invoke(b, null));
        Console.WriteLine(typeof(Base).IsAssignableFrom(typeof(Derived)) + " " + (b is Derived) + " " + typeof(IComparable).IsAssignableFrom(typeof(int)));

        int? n = null; int? m = 5;
        Console.WriteLine(n.HasValue + " " + (m + 1) + " " + (n ?? 7) + " " + ((object)m).GetType().Name);

        var v = new Vec3(1, 2, 3) + new Vec3(0.5f, 0.5f, 0.5f) * 2;
        Console.WriteLine(v + " " + v.Dot(new Vec3(1, 0, 0)));
        Console.WriteLine(Mathf(2.5f));

        var threads = new List<Thread>();
        for (int i = 0; i < 4; i++) { var th = new Thread(Work); threads.Add(th); th.Start(); }
        foreach (var th in threads) th.Join();
        Console.WriteLine(counter);
        int shared = 0;
        var done = new ManualResetEvent(false);
        ThreadPool.QueueUserWorkItem(_ => { Interlocked.Add(ref shared, 5); done.Set(); });
        done.WaitOne();
        Console.WriteLine(shared);

        Console.WriteLine(Order());

        string path = Path.Combine(Path.GetTempPath(), "netrecomp_conf.txt");
        File.WriteAllText(path, "line one\nline two\n");
        var lines = File.ReadAllLines(path);
        Console.WriteLine(lines.Length + lines[1]);
        using (var fs = new FileStream(path, FileMode.Append)) { var bytes = new byte[] { 65, 66 }; fs.Write(bytes, 0, 2); }
        Console.WriteLine(new FileInfo(path).Length);
        File.Delete(path);
        Console.WriteLine(File.Exists(path));
        Guid g1 = Guid.NewGuid(), g2 = Guid.NewGuid();
        Console.WriteLine((g1 != Guid.Empty) + " " + (g1 != g2) + " " + g1.ToString().Length);
        return 0;
    }

    static float Mathf(float f) { return (float)Math.Floor(f) + (float)Math.Round(f) + Math.Abs(-f) + (float)Math.Pow(f, 2); }
}
