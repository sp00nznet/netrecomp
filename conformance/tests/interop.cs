// P/Invoke: stdcall imports, a cdecl export declared with the default
// (stdcall) convention, string and array marshalling, bool returns, and a
// managed callback called from native code (qsort).
using System;
using System.Runtime.InteropServices;
using System.Text;

static class Program
{
    [DllImport("kernel32.dll")] static extern int MulDiv(int a, int b, int c);
    [DllImport("kernel32.dll")] static extern int GetModuleHandle(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern int lstrlenW(string s);
    [DllImport("kernel32.dll")] [return: MarshalAs(UnmanagedType.Bool)] static extern bool QueryPerformanceFrequency(out long f);
    // msvcrt exports cdecl; these say nothing, so they are declared stdcall
    [DllImport("msvcrt.dll")] static extern int strlen(string s);
    [DllImport("msvcrt.dll")] static extern int abs(int v);
    [DllImport("msvcrt.dll")] static extern double fabs(double v);
    [DllImport("msvcrt.dll", CallingConvention = CallingConvention.Cdecl)] static extern int memcmp(byte[] a, byte[] b, int n);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Compare(IntPtr a, IntPtr b);
    [DllImport("msvcrt.dll", CallingConvention = CallingConvention.Cdecl)] static extern void qsort(int[] data, int n, int size, Compare cmp);

    static int calls;
    static int Desc(IntPtr a, IntPtr b)
    {
        calls++;
        int x = Marshal.ReadInt32(a), y = Marshal.ReadInt32(b);
        return y.CompareTo(x);
    }

    static int Main()
    {
        Console.WriteLine(MulDiv(300, 7, 4));
        Console.WriteLine(GetModuleHandle("kernel32.dll") != 0);
        Console.WriteLine(lstrlenW("wide string"));
        long f; Console.WriteLine(QueryPerformanceFrequency(out f) && f > 0);
        Console.WriteLine(strlen("hello") + abs(-42) + (int)fabs(-2.5));
        Console.WriteLine(memcmp(new byte[] { 1, 2, 3 }, new byte[] { 1, 2, 4 }, 3) < 0);
        int[] data = { 5, 1, 9, 3, 7, 2 };
        Compare cmp = Desc;
        qsort(data, data.Length, 4, cmp);
        Console.WriteLine(string.Join(",", Array.ConvertAll(data, x => x.ToString())) + " " + (calls > 0));
        IntPtr mem = Marshal.AllocHGlobal(16);
        Marshal.WriteInt32(mem, 4, 1234);
        Console.WriteLine(Marshal.ReadInt32(mem, 4));
        Marshal.FreeHGlobal(mem);
        return 0;
    }
}
