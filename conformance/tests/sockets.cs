// Loopback only: TCP (sync + Begin/End async), UDP, Poll/Select, Dns on literals.
using System;
using System.Collections;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

static class P
{
    static int Main()
    {
        var lo = IPAddress.Parse("127.0.0.1");
        var l = new TcpListener(lo, 0);
        l.Start();
        int port = ((IPEndPoint)l.LocalEndpoint).Port;
        Console.WriteLine(port > 0);
        var t = new Thread(() =>
        {
            using (var c = l.AcceptTcpClient())
            {
                var s = c.GetStream();
                var b = new byte[64];
                int n = s.Read(b, 0, b.Length);
                var reply = Encoding.ASCII.GetBytes("echo:" + Encoding.ASCII.GetString(b, 0, n));
                s.Write(reply, 0, reply.Length);
            }
        });
        t.Start();
        var cl = new TcpClient();
        cl.Connect(lo, port);
        var st = cl.GetStream();
        var msg = Encoding.ASCII.GetBytes("hello");
        st.Write(msg, 0, msg.Length);
        var buf = new byte[64];
        var ar = st.BeginRead(buf, 0, buf.Length, null, null);
        int got = st.EndRead(ar);
        Console.WriteLine(Encoding.ASCII.GetString(buf, 0, got));
        t.Join();
        cl.Close();
        l.Stop();

        var u1 = new UdpClient(new IPEndPoint(lo, 0));
        var u2 = new UdpClient();
        u2.Send(Encoding.ASCII.GetBytes("dgram"), 5, (IPEndPoint)u1.Client.LocalEndPoint);
        Console.WriteLine(u1.Client.Poll(2000000, SelectMode.SelectRead));
        var rl = new ArrayList { u1.Client };
        Socket.Select(rl, null, null, 1000000);
        Console.WriteLine(rl.Count);
        var from = new IPEndPoint(IPAddress.Any, 0);
        Console.WriteLine(Encoding.ASCII.GetString(u1.Receive(ref from)) + " " + from.Address);
        u1.Close(); u2.Close();

        Console.WriteLine(Dns.GetHostEntry("127.0.0.1").AddressList[0]);
        try { new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp).Connect(lo, 1); Console.WriteLine("connected?"); }
        catch (SocketException e) { Console.WriteLine(e.SocketErrorCode); }
        return 5;
    }
}
