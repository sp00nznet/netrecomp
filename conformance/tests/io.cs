// File system queries: Exists on files, directories and missing paths,
// Path helpers, and a write/read round trip.
using System;
using System.IO;

static class Program
{
    static int Main()
    {
        string dir = Path.Combine(Path.GetTempPath(), "nr_io_test");
        Directory.CreateDirectory(dir);
        string file = Path.Combine(dir, "a.txt");
        File.WriteAllText(file, "hello\nworld");
        Console.WriteLine(File.Exists(file));
        Console.WriteLine(File.Exists(dir));
        Console.WriteLine(File.Exists(dir + "/missing/b.assetbundle"));
        Console.WriteLine(File.Exists(dir + "/missing.txt"));
        Console.WriteLine(File.Exists(""));
        Console.WriteLine(Directory.Exists(dir));
        Console.WriteLine(Directory.Exists(file));
        Console.WriteLine(Directory.Exists(dir + "/nope"));
        Console.WriteLine(File.ReadAllLines(file).Length);
        Console.WriteLine(new FileInfo(file).Length);
        Console.WriteLine(Path.GetFileNameWithoutExtension(file) + Path.GetExtension(file));
        File.Delete(file);
        Console.WriteLine(File.Exists(file));
        Directory.Delete(dir);
        return 0;
    }
}
