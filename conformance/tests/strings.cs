// String building and formatting as game code uses it: Format with string
// and numeric args, Concat, Replace, Split, case, Contains, comparisons.
using System;
using System.Text;

static class Program
{
    static int Main()
    {
        Console.WriteLine(string.Format("UI/MainMenu/{0}{1}", "mainMenuLogo", "-SRR"));
        Console.WriteLine(string.Format("{0}:{1,5}|{2,-4}|{3:0.0###}", "a", 42, "x", 1.27f));
        Console.WriteLine(string.Format("{0:X8} {1:N2}", 255, 1234.5));
        Console.WriteLine(string.Concat("a", "b", "c", "d") + string.Concat(new object[] { 1, 'x', 2.5 }));
        Console.WriteLine("Seattle-TitleTheme".Replace("Seattle", "Berlin").ToUpper());
        Console.WriteLine(string.Join("|", "a,b,,c".Split(',')));
        Console.WriteLine("GroundPlane(Clone)".Contains("GroundPlane(") + " " + "abc".StartsWith("ab") + " " + "abc".EndsWith("bc"));
        Console.WriteLine(string.Compare("apple", "Banana", StringComparison.OrdinalIgnoreCase) < 0);
        string schema = "characters/humanfemale/JessicaWattsUB";
        Console.WriteLine(schema.EndsWith("jessicawattsub", StringComparison.OrdinalIgnoreCase) + " " + schema.EndsWith("JessicaWattsUB", StringComparison.Ordinal)
            + " " + schema.StartsWith("CHARACTERS", StringComparison.OrdinalIgnoreCase) + " " + schema.IndexOf("WATTS", StringComparison.OrdinalIgnoreCase)
            + " " + schema.EndsWith("jessicawattsub", StringComparison.CurrentCultureIgnoreCase) + " " + schema.EndsWith("x", StringComparison.OrdinalIgnoreCase));
        Console.WriteLine("  trim me ".Trim() + "|" + "x".PadLeft(3, '.') + "|" + "abcdef".Substring(2, 3));
        var sb = new StringBuilder();
        for (int i = 0; i < 5; i++) sb.Append(i).Append(',');
        sb.AppendFormat("{0}-{1}", "end", 7);
        Console.WriteLine(sb.ToString());
        Console.WriteLine(1.5f.ToString() + " " + (-3).ToString() + " " + 1e10.ToString() + " " + 0.1.ToString());
        Console.WriteLine(int.Parse("123") + float.Parse("2.5") + double.Parse("1e2"));
        Console.WriteLine("CONTINUE".ToLowerInvariant() + " " + "x".Equals("X", StringComparison.OrdinalIgnoreCase));
        return 0;
    }
}
