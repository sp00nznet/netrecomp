// Core semantics: arithmetic, strings, objects, virtual and interface dispatch,
// generics, structs, boxing, arrays, exceptions, delegates, statics.
// Output must match the same program run on the real CLR.
using System;
using System.Collections.Generic;
using System.Text;

interface IShape { double Area(); string Name { get; } }
abstract class Shape : IShape
{
    public abstract double Area();
    public virtual string Name { get { return "shape"; } }
    public override string ToString() { return Name + ":" + Area().ToString("F2"); }
}
class Circle : Shape
{
    double r;
    public Circle(double r) { this.r = r; }
    public override double Area() { return Math.PI * r * r; }
    public override string Name { get { return "circle"; } }
}
class Rect : Shape
{
    protected double w, h;
    public Rect(double w, double h) { this.w = w; this.h = h; }
    public override double Area() { return w * h; }
    public override string Name { get { return "rect"; } }
}
sealed class Square : Rect { public Square(double s) : base(s, s) { } public override string Name { get { return "square"; } } }

struct Point : IComparable<Point>
{
    public int X, Y;
    public Point(int x, int y) { X = x; Y = y; }
    public int CompareTo(Point o) { return X != o.X ? X.CompareTo(o.X) : Y.CompareTo(o.Y); }
    public override string ToString() { return "(" + X + "," + Y + ")"; }
}

enum Color { Red, Green = 5, Blue }

static class Counter
{
    public static int Count;
    static Counter() { Count = 100; Console.WriteLine("Counter cctor"); }
    public static int Next() { return ++Count; }
}

class Node<T>
{
    public T Value; public Node<T> Next;
    public Node(T v, Node<T> n) { Value = v; Next = n; }
}

delegate int BinOp(int a, int b);

class MyException : Exception { public int Code; public MyException(string m, int c) : base(m) { Code = c; } }

static class Program
{
    static int Fib(int n) { return n < 2 ? n : Fib(n - 1) + Fib(n - 2); }
    static T Max<T>(T a, T b) where T : IComparable<T> { return a.CompareTo(b) >= 0 ? a : b; }
    static int Sum(params int[] xs) { int s = 0; foreach (var x in xs) s += x; return s; }

    static IEnumerable<int> Evens(int n)
    {
        for (int i = 0; i < n; i++)
            if (i % 2 == 0) yield return i;
    }

    static string Finally(int mode)
    {
        var sb = new StringBuilder();
        try
        {
            sb.Append("try;");
            if (mode == 1) throw new InvalidOperationException("boom");
            if (mode == 2) return sb.Append("ret;").ToString();
        }
        catch (InvalidOperationException e) { sb.Append("catch " + e.Message + ";"); }
        finally { sb.Append("finally;"); }
        return sb.ToString();
    }

    static int Main()
    {
        Console.WriteLine("hello, netrecomp");
        int a = 7, b = -3;
        Console.WriteLine(a + b); Console.WriteLine(a * b); Console.WriteLine(a / b); Console.WriteLine(a % b);
        Console.WriteLine(a << 3); Console.WriteLine(b >> 1); Console.WriteLine((uint)b >> 1);
        long big = 1L << 40; Console.WriteLine(big * 3 + 1);
        Console.WriteLine(unchecked(int.MaxValue + 1));
        double d = 1.0 / 3; Console.WriteLine(d.ToString("R")); Console.WriteLine((float)d);
        Console.WriteLine((int)3.99); Console.WriteLine((int)-3.99); Console.WriteLine(Math.Sqrt(2.0));
        Console.WriteLine(Fib(20));

        string s = "Hello" + ", " + "World";
        Console.WriteLine(s.Length); Console.WriteLine(s.ToUpper()); Console.WriteLine(s.Substring(7));
        Console.WriteLine(s.IndexOf("World")); Console.WriteLine(s.Replace('l', 'L'));
        Console.WriteLine(string.Format("{0}-{1:X4}-{2,5}|", "fmt", 255, 42));
        Console.WriteLine(string.Join(",", new[] { "a", "b", "c" }));
        Console.WriteLine("abc".CompareTo("abd")); Console.WriteLine(s.GetHashCode() == ("Hello, " + "World").GetHashCode());

        var shapes = new List<IShape> { new Circle(1), new Rect(2, 3), new Square(4) };
        foreach (var sh in shapes) Console.WriteLine(sh.ToString());
        double total = 0; foreach (var sh in shapes) total += sh.Area();
        Console.WriteLine(total.ToString("F3"));
        object o = shapes[2];
        Console.WriteLine(o is Rect); Console.WriteLine(o is Circle); Console.WriteLine((o as IShape).Name);

        var p = new Point(3, 4); object boxed = p; var q = (Point)boxed; q.X = 9;
        Console.WriteLine(p); Console.WriteLine(q); Console.WriteLine(boxed);
        Console.WriteLine(Max(new Point(1, 9), new Point(2, 0)));
        Console.WriteLine(Max(3, 8)); Console.WriteLine(Max("pear", "apple"));
        var pts = new Point[3]; pts[1].Y = 5; Console.WriteLine(pts[1]);

        Color c = Color.Blue; Console.WriteLine(c); Console.WriteLine((int)c); Console.WriteLine(Color.Green.ToString());

        Console.WriteLine(Counter.Next()); Console.WriteLine(Counter.Next());

        Node<string> list = null;
        foreach (var w in "the quick brown fox".Split(' ')) list = new Node<string>(w, list);
        for (var n = list; n != null; n = n.Next) Console.Write(n.Value + " ");
        Console.WriteLine();

        var dict = new Dictionary<string, int>();
        foreach (var w in "a b a c b a".Split(' ')) { int v; dict.TryGetValue(w, out v); dict[w] = v + 1; }
        var keys = new List<string>(dict.Keys); keys.Sort();
        foreach (var k in keys) Console.WriteLine(k + "=" + dict[k]);

        int[] arr = { 5, 3, 9, 1, 7 }; Array.Sort(arr); Console.WriteLine(string.Join(" ", Array.ConvertAll(arr, x => x.ToString())));
        int[,] grid = new int[3, 4]; grid[2, 3] = 42; Console.WriteLine(grid[2, 3] + grid.GetLength(1));
        Console.WriteLine(Sum(1, 2, 3, 4));
        foreach (var e in Evens(7)) Console.Write(e); Console.WriteLine();

        BinOp add = (x, y) => x + y; BinOp mul = delegate (int x, int y) { return x * y; };
        Console.WriteLine(add(2, 3) + mul(4, 5));
        int captured = 10; Func<int, int> addc = x => x + captured; captured = 20; Console.WriteLine(addc(1));
        Action multi = () => Console.Write("one "); multi += () => Console.Write("two "); multi(); Console.WriteLine();

        Console.WriteLine(Finally(0)); Console.WriteLine(Finally(1)); Console.WriteLine(Finally(2));
        try { throw new MyException("mine", 7); }
        catch (MyException e) { Console.WriteLine(e.Message + e.Code); }
        try { object n = null; n.ToString(); }
        catch (NullReferenceException) { Console.WriteLine("caught NRE"); }
        try { int z = 0; Console.WriteLine(10 / z); }
        catch (DivideByZeroException) { Console.WriteLine("caught div0"); }
        try { var x = new int[2]; x[5] = 1; }
        catch (IndexOutOfRangeException) { Console.WriteLine("caught range"); }
        try { object x = "s"; var y = (Shape)x; }
        catch (InvalidCastException) { Console.WriteLine("caught cast"); }
        try { checked { int x = int.MaxValue; x++; } }
        catch (OverflowException) { Console.WriteLine("caught overflow"); }

        var sbig = new StringBuilder();
        for (int i = 0; i < 1000; i++) sbig.Append(i % 10);
        Console.WriteLine(sbig.Length);
        var garbage = new List<object>();
        for (int i = 0; i < 200000; i++) { garbage.Add(new int[16]); if (garbage.Count > 1000) garbage.Clear(); }
        Console.WriteLine("gc ok");
        return 3;
    }
}
