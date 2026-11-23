// Decimal parse and arithmetic (the corlib's decimal internal calls).
using System;
using System.Globalization;

static class P
{
    static int Main()
    {
        var c = CultureInfo.InvariantCulture;
        decimal a = decimal.Parse("12.5", c), b = decimal.Parse("-0.25", c);
        Console.WriteLine((a + b).ToString(c));
        Console.WriteLine((a * b).ToString(c));
        Console.WriteLine((a / b).ToString(c));
        Console.WriteLine(decimal.Compare(a, b) + " " + Math.Floor(b).ToString(c) + " " + decimal.Truncate(a).ToString(c));
        Console.WriteLine((double)a + " " + (long)a + " " + decimal.Parse("1.5e3", NumberStyles.Float, c).ToString(c));
        return 0;
    }
}
