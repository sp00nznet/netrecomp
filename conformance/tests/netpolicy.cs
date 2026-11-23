// Network policy fails closed: with NR_NET_ALLOW unset, a recompiled program reaches
// loopback only. The CLR oracle prints the expected lines without touching the network
// (it would really connect), so only the recompiled run exercises the policy.
using System;
using System.Net;
using System.Net.Sockets;

static class P
{
    static bool Refused(Action a)
    {
        try { a(); return false; } catch (SocketException e) { Console.Error.WriteLine("  " + e.SocketErrorCode); return true; }
    }

    static int Main()
    {
        bool recompiled = Environment.Version.Major == 2 || typeof(object).Assembly.GetType("Mono.Runtime") != null;   // mono class library: the recompiled run
        if (!recompiled || Environment.GetEnvironmentVariable("NR_NET_ALLOW") != null)
        {
            Console.WriteLine("remote refused: True");
            Console.WriteLine("lookup refused: True");
            Console.WriteLine("loopback: True");
            return 0;
        }
        // TEST-NET-1 (RFC 5737): never routed, so even a policy failure goes nowhere
        Console.WriteLine("remote refused: " + Refused(() =>
        {
            var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            s.Connect(new IPEndPoint(IPAddress.Parse("192.0.2.1"), 9));
        }));
        // .invalid (RFC 2606) never resolves, but the lookup must not even be sent
        Console.WriteLine("lookup refused: " + Refused(() => Dns.GetHostEntry("policy-check.invalid")));
        var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        var c = new TcpClient();
        c.Connect(IPAddress.Loopback, ((IPEndPoint)l.LocalEndpoint).Port);
        Console.WriteLine("loopback: " + c.Connected);
        c.Close(); l.Stop();
        return 0;
    }
}
