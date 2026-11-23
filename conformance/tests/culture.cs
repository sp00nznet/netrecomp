// A named culture's time patterns (DateTime.ToString under en-US).
using System;
using System.Globalization;

static class P
{
    static int Main()
    {
        var ci = new CultureInfo("en-US");
        var t = new DateTime(2020, 1, 2, 15, 4, 5);
        Console.WriteLine(t.ToString("T", ci));
        Console.WriteLine(t.ToString("t", ci));
        Console.WriteLine(ci.DateTimeFormat.LongTimePattern);
        return 0;
    }
}
