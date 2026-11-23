/* nr_net.c: System.Net.Sockets.Socket and System.Net.Dns internal calls on WinSock
 * (mono 2.0 profile). Errors go back as WSA codes, which are SocketError's values.
 * A SocketAddress is its byte[] "data": family (2 bytes, little endian), then the rest
 * of a sockaddr, which is exactly WinSock's layout.
 *
 * NR_NET_ALLOW=<ipv4>[,<ipv4>...] limits connect/sendto to those addresses (automated
 * runs set it to 127.0.0.1 so nothing leaves the machine). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "nrrt.h"

#define ARR(o) ((NrArray*)(o))

static NrClass *k_sockaddr, *k_socket;
static int32_t off_data, off_sock;

static int32_t foff(NrClass* k, const char* n) { const NrFieldDesc* f = k ? nr_field(k, n) : 0; return f ? f->offset : -1; }

static void* sa_new(const struct sockaddr* a, int len)
{
    if (!k_sockaddr || off_data < 0) return 0;
    void* o = nr_new(k_sockaddr);
    void* d = nr_newarr(nr_array_class(nr_k_byte, 1), len);
    memcpy(ARR(d)->vector, a, (size_t)len);
    *(void**)((char*)o + off_data) = d;
    return o;
}
static int sa_get(void* o, struct sockaddr_storage* ss)
{
    if (!o || off_data < 0) return 0;
    NrArray* d = *(NrArray**)((char*)o + off_data);
    int n = d ? (int)d->max_length : 0;
    if (n > (int)sizeof *ss) n = sizeof *ss;
    memset(ss, 0, sizeof *ss);
    if (n) memcpy(ss, d->vector, (size_t)n);
    return n;
}

static int allowed(const struct sockaddr* a)
{
    const char* allow = getenv("NR_NET_ALLOW");
    if (!allow || !*allow) return 1;
    if (a->sa_family != AF_INET) return 0;
    char ip[64];
    inet_ntop(AF_INET, &((const struct sockaddr_in*)a)->sin_addr, ip, sizeof ip);
    size_t n = strlen(ip);
    for (const char* p = allow; (p = strstr(p, ip)); p += n)
        if ((p == allow || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
    nr_log("netrecomp: blocked connection to %s (NR_NET_ALLOW)\n", ip);
    return 0;
}

#define ERR(e) (*(e) = WSAGetLastError())

/* not at registration: that can run under the loader lock, where WSAStartup deadlocks */
static void wsa(void)
{
    static volatile LONG done;
    if (done) return;
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);   /* refcounted, so a racing second call is harmless */
    done = 1;
}

/* an instance icall in Unity's System.dll */
static intptr_t Socket_internal(void* self, int32_t af, int32_t type, int32_t proto, int32_t* e)
{
    (void)self;
    wsa();
    *e = 0;
    SOCKET s = socket(af, type, proto);
    if (s == INVALID_SOCKET) { ERR(e); return -1; }
    return (intptr_t)s;
}
static void Close_internal(intptr_t h, int32_t* e) { *e = closesocket((SOCKET)h) ? WSAGetLastError() : 0; }
static void Blocking_internal(intptr_t h, uint8_t block, int32_t* e)
{
    u_long nb = !block;
    *e = ioctlsocket((SOCKET)h, FIONBIO, &nb) ? WSAGetLastError() : 0;
}
static void Connect_internal(intptr_t h, void* sa, int32_t* e)
{
    struct sockaddr_storage ss; int n = sa_get(sa, &ss);
    if (!allowed((struct sockaddr*)&ss)) { *e = WSAEACCES; return; }
    *e = connect((SOCKET)h, (struct sockaddr*)&ss, n) ? WSAGetLastError() : 0;
}
static void Bind_internal(intptr_t h, void* sa, int32_t* e)
{
    struct sockaddr_storage ss; int n = sa_get(sa, &ss);
    *e = bind((SOCKET)h, (struct sockaddr*)&ss, n) ? WSAGetLastError() : 0;
}
static void Listen_internal(intptr_t h, int32_t backlog, int32_t* e) { *e = listen((SOCKET)h, backlog) ? WSAGetLastError() : 0; }
static intptr_t Accept_internal(intptr_t h, int32_t* e, uint8_t blocking)
{
    (void)blocking;
    *e = 0;
    SOCKET s = accept((SOCKET)h, 0, 0);
    if (s == INVALID_SOCKET) { ERR(e); return -1; }
    return (intptr_t)s;
}
static int32_t Available_internal(intptr_t h, int32_t* e)
{
    u_long n = 0;
    *e = ioctlsocket((SOCKET)h, FIONREAD, &n) ? WSAGetLastError() : 0;
    return (int32_t)n;
}
static int range_ok(void* buf, int32_t off, int32_t count, int32_t* e)
{
    if (!buf || off < 0 || count < 0 || (uint32_t)off + (uint32_t)count > ARR(buf)->max_length) { *e = WSAEFAULT; return 0; }
    return 1;
}
static int32_t Send_internal(intptr_t h, void* buf, int32_t off, int32_t count, int32_t flags, int32_t* e)
{
    *e = 0;
    if (!range_ok(buf, off, count, e)) return 0;
    int r = send((SOCKET)h, (char*)ARR(buf)->vector + off, count, flags);
    if (r < 0) { ERR(e); return 0; }
    return r;
}
static int32_t Receive_internal(intptr_t h, void* buf, int32_t off, int32_t count, int32_t flags, int32_t* e)
{
    *e = 0;
    if (!range_ok(buf, off, count, e)) return 0;
    int r = recv((SOCKET)h, (char*)ARR(buf)->vector + off, count, flags);
    if (r < 0) { ERR(e); return 0; }
    return r;
}
static int32_t SendTo_internal(intptr_t h, void* buf, int32_t off, int32_t count, int32_t flags, void* sa, int32_t* e)
{
    *e = 0;
    if (!range_ok(buf, off, count, e)) return 0;
    struct sockaddr_storage ss; int n = sa_get(sa, &ss);
    if (!allowed((struct sockaddr*)&ss)) { *e = WSAEACCES; return 0; }
    int r = sendto((SOCKET)h, (char*)ARR(buf)->vector + off, count, flags, (struct sockaddr*)&ss, n);
    if (r < 0) { ERR(e); return 0; }
    return r;
}
static int32_t RecvFrom_internal(intptr_t h, void* buf, int32_t off, int32_t count, int32_t flags, void** sa, int32_t* e)
{
    *e = 0;
    if (!range_ok(buf, off, count, e)) return 0;
    struct sockaddr_storage ss; int n = sizeof ss;
    int r = recvfrom((SOCKET)h, (char*)ARR(buf)->vector + off, count, flags, (struct sockaddr*)&ss, &n);
    if (r < 0) { ERR(e); *sa = 0; return 0; }
    *sa = sa_new((struct sockaddr*)&ss, n);
    return r;
}
static void* LocalEndPoint_internal(intptr_t h, int32_t* e)
{
    struct sockaddr_storage ss; int n = sizeof ss;
    if (getsockname((SOCKET)h, (struct sockaddr*)&ss, &n)) { ERR(e); return 0; }
    *e = 0;
    return sa_new((struct sockaddr*)&ss, n);
}
static void* RemoteEndPoint_internal(intptr_t h, int32_t* e)
{
    struct sockaddr_storage ss; int n = sizeof ss;
    if (getpeername((SOCKET)h, (struct sockaddr*)&ss, &n)) { ERR(e); return 0; }
    *e = 0;
    return sa_new((struct sockaddr*)&ss, n);
}
static uint8_t Poll_internal(intptr_t h, int32_t mode, int32_t us, int32_t* e)
{
    fd_set s; FD_ZERO(&s); FD_SET((SOCKET)h, &s);
    struct timeval tv = { us / 1000000, us % 1000000 };
    int r = select(0, mode == 0 ? &s : 0, mode == 1 ? &s : 0, mode == 2 ? &s : 0, us < 0 ? 0 : &tv);
    if (r < 0) { ERR(e); return 0; }
    *e = 0;
    return r > 0;
}
/* sockets: read list, null, write list, null, error list, null; on return each list
 * keeps only the ready sockets (mono's layout) */
static void Select_internal(void** psockets, int32_t us, int32_t* e)
{
    void* arr = *psockets;
    *e = 0;
    if (!arr || off_sock < 0) return;
    void** v = (void**)ARR(arr)->vector;
    int n = (int)ARR(arr)->max_length;
    fd_set sets[3]; for (int i = 0; i < 3; i++) FD_ZERO(&sets[i]);
    for (int i = 0, m = 0; i < n && m < 3; i++)
    {
        if (!v[i]) { m++; continue; }
        if (sets[m].fd_count < FD_SETSIZE) FD_SET((SOCKET)*(intptr_t*)((char*)v[i] + off_sock), &sets[m]);
    }
    struct timeval tv = { us / 1000000, us % 1000000 };
    int r = select(0, &sets[0], &sets[1], &sets[2], us < 0 ? 0 : &tv);
    if (r < 0) { ERR(e); return; }
    int cnt = 0;
    for (int i = 0, m = 0; i < n && m < 3; i++)
    {
        if (!v[i]) { m++; cnt++; continue; }
        if (FD_ISSET((SOCKET)*(intptr_t*)((char*)v[i] + off_sock), &sets[m])) cnt++;
    }
    void* out = nr_newarr(ARR(arr)->o.klass, cnt);
    void** w = (void**)ARR(out)->vector;
    for (int i = 0, m = 0, j = 0; i < n && m < 3; i++)
    {
        if (!v[i]) { m++; w[j++] = 0; continue; }
        if (FD_ISSET((SOCKET)*(intptr_t*)((char*)v[i] + off_sock), &sets[m])) w[j++] = v[i];
    }
    *psockets = out;
}
static void Shutdown_internal(intptr_t h, int32_t how, int32_t* e) { *e = shutdown((SOCKET)h, how) ? WSAGetLastError() : 0; }
static void Disconnect_internal(intptr_t h, uint8_t reuse, int32_t* e) { (void)reuse; *e = shutdown((SOCKET)h, SD_BOTH) ? WSAGetLastError() : 0; }

static int32_t ifield(void* o, const char* name, int32_t dflt)
{
    int32_t off = foff(((NrObj*)o)->klass, name);
    return off < 0 ? dflt : *(int32_t*)((char*)o + off);
}
static void SetSocketOption_internal(intptr_t h, int32_t level, int32_t name, void* obj, void* bytes, int32_t ival, int32_t* e)
{
    int r;
    if (bytes) r = setsockopt((SOCKET)h, level, name, (char*)ARR(bytes)->vector, (int)ARR(bytes)->max_length);
    else if (obj && !strcmp(((NrObj*)obj)->klass->name, "LingerOption"))
    {
        /* LingerOption { bool enabled; int seconds; } */
        int32_t eo = foff(((NrObj*)obj)->klass, "enabled");
        struct linger l = { (u_short)(eo >= 0 && *((uint8_t*)obj + eo)), (u_short)ifield(obj, "seconds", 0) };
        r = setsockopt((SOCKET)h, level, name, (char*)&l, sizeof l);
    }
    else if (obj) { *e = WSAENOPROTOOPT; return; }   /* multicast options: not needed yet */
    else r = setsockopt((SOCKET)h, level, name, (char*)&ival, sizeof ival);
    *e = r ? WSAGetLastError() : 0;
}
static void GetSocketOption_obj_internal(intptr_t h, int32_t level, int32_t name, void** val, int32_t* e)
{
    int32_t v = 0; int n = sizeof v;
    if (getsockopt((SOCKET)h, level, name, (char*)&v, &n)) { ERR(e); *val = 0; return; }
    *e = 0;
    *val = nr_box(nr_k_int32, &v);
}
static void GetSocketOption_arr_internal(intptr_t h, int32_t level, int32_t name, void** bytes, int32_t* e)
{
    void* b = *bytes;
    int n = b ? (int)ARR(b)->max_length : 0;
    *e = getsockopt((SOCKET)h, level, name, b ? (char*)ARR(b)->vector : 0, &n) ? WSAGetLastError() : 0;
}

/* ---- Dns ---- */

static void* str_array(char** v, int n)
{
    void* a = nr_new_string_array(n);
    for (int i = 0; i < n; i++) ((void**)ARR(a)->vector)[i] = nr_str_utf8(v[i]);
    return a;
}
static uint8_t fill_host(struct addrinfo* res, const char* name, void** h_name, void** aliases, void** addrs)
{
    char* list[64]; char buf[64][INET6_ADDRSTRLEN]; int n = 0;
    for (struct addrinfo* p = res; p && n < 64; p = p->ai_next)
    {
        if (p->ai_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in*)p->ai_addr)->sin_addr, buf[n], sizeof buf[n]);
        else if (p->ai_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6*)p->ai_addr)->sin6_addr, buf[n], sizeof buf[n]);
        else continue;
        int dup = 0;
        for (int j = 0; j < n; j++) dup |= !strcmp(buf[j], buf[n]);
        if (!dup) { list[n] = buf[n]; n++; }
    }
    *h_name = nr_str_utf8(res && res->ai_canonname ? res->ai_canonname : name);
    *aliases = nr_new_string_array(0);
    *addrs = str_array(list, n);
    return 1;
}
static uint8_t Dns_GetHostByName_internal(void* host, void** h_name, void** aliases, void** addrs)
{
    wsa();
    char* h = nr_utf8(host);
    struct addrinfo hints = { 0 }, *res = 0;
    hints.ai_family = AF_INET;   /* mono 2.0 callers expect IPv4 first; IPv6 off by default */
    hints.ai_flags = AI_CANONNAME;
    int r = getaddrinfo(h, 0, &hints, &res);
    uint8_t ok = r == 0 && fill_host(res, h, h_name, aliases, addrs);
    if (res) freeaddrinfo(res);
    nr_free(h);
    return ok;
}
static uint8_t Dns_GetHostByAddr_internal(void* addr, void** h_name, void** aliases, void** addrs)
{
    wsa();
    char* a = nr_utf8(addr);
    struct addrinfo hints = { 0 }, *res = 0;
    hints.ai_flags = AI_NUMERICHOST;
    int r = getaddrinfo(a, 0, &hints, &res);
    uint8_t ok = 0;
    if (!r)
    {
        char host[NI_MAXHOST];
        /* no reverse lookup: the numeric name answers (keeps lookups local) */
        getnameinfo(res->ai_addr, (int)res->ai_addrlen, host, sizeof host, 0, 0, NI_NUMERICHOST);
        fill_host(res, host, h_name, aliases, addrs);
        ok = 1;
    }
    if (res) freeaddrinfo(res);
    nr_free(a);
    return ok;
}
static void* W32ErrorMessage(int32_t code)
{
    char b[512];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0, (DWORD)code, 0, b, sizeof b, 0);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r' || b[n - 1] == '.')) b[--n] = 0;
    if (!n) _snprintf(b, sizeof b, "error %d", code);
    return nr_str_utf8(b);
}
static uint8_t Env_get_SocketSecurityEnabled(void) { return 0; }
static uint8_t Dns_GetHostName_internal(void** name)
{
    wsa();
    char b[256];
    if (gethostname(b, sizeof b)) return 0;
    *name = nr_str_utf8(b);
    return 1;
}

/* Socket async Receive (2) and Send (4): wait until ready, transfer, record total and
 * error on the SocketAsyncResult; the managed worker then completes it. */
void nr_socket_aio(void* r)
{
    static int32_t o_op = -2, o_h, o_buf, o_off, o_size, o_fl, o_total, o_err;
    if (!r) return;
    if (o_op == -2)
    {
        NrClass* c = ((NrObj*)r)->klass;
        o_h = foff(c, "handle"); o_buf = foff(c, "Buffer"); o_off = foff(c, "Offset"); o_size = foff(c, "Size");
        o_fl = foff(c, "SockFlags"); o_total = foff(c, "total"); o_err = foff(c, "error"); o_op = foff(c, "operation");
    }
    if (o_op < 0 || o_h < 0 || o_buf < 0 || o_total < 0 || o_err < 0) return;
    char* b = (char*)r;
    int32_t op = *(int32_t*)(b + o_op);
    if (op != 2 && op != 4) return;
    intptr_t h = *(intptr_t*)(b + o_h);
    fd_set s; FD_ZERO(&s); FD_SET((SOCKET)h, &s);
    select(0, op == 2 ? &s : 0, op == 4 ? &s : 0, 0, 0);
    int32_t e = 0;
    void* buf = *(void**)(b + o_buf);
    int32_t off = *(int32_t*)(b + o_off), size = *(int32_t*)(b + o_size), fl = *(int32_t*)(b + o_fl);
    int32_t t = op == 2 ? Receive_internal(h, buf, off, size, fl, &e) : Send_internal(h, buf, off, size, fl, &e);
    *(int32_t*)(b + o_total) = t;
    *(int32_t*)(b + o_err) = e;
}

void nr_register_net_icalls(void)
{
    k_sockaddr = nr_class_by_name("System.Net", "SocketAddress");
    k_socket = nr_class_by_name("System.Net.Sockets", "Socket");
    off_data = foff(k_sockaddr, "data");
    off_sock = foff(k_socket, "socket");
#define S(n, f) { "System.Net.Sockets.Socket::" n, (void*)f }
    static const NrIcall t[] = {
        S("Socket_internal", Socket_internal), S("Close_internal", Close_internal), S("Blocking_internal", Blocking_internal),
        S("Connect_internal", Connect_internal), S("Connect_internal_real", Connect_internal), S("Bind_internal", Bind_internal),
        S("Listen_internal", Listen_internal), S("Accept_internal", Accept_internal), S("Available_internal", Available_internal),
        S("Send_internal(intptr,byte[],int,int,System.Net.Sockets.SocketFlags,int&)", Send_internal),
        S("Receive_internal(intptr,byte[],int,int,System.Net.Sockets.SocketFlags,int&)", Receive_internal),
        S("SendTo_internal", SendTo_internal), S("SendTo_internal_real", SendTo_internal), S("RecvFrom_internal", RecvFrom_internal),
        S("LocalEndPoint_internal", LocalEndPoint_internal), S("RemoteEndPoint_internal", RemoteEndPoint_internal),
        S("Poll_internal", Poll_internal), S("Select_internal", Select_internal), S("Shutdown_internal", Shutdown_internal),
        S("Disconnect_internal", Disconnect_internal), S("SetSocketOption_internal", SetSocketOption_internal),
        S("GetSocketOption_obj_internal", GetSocketOption_obj_internal), S("GetSocketOption_arr_internal", GetSocketOption_arr_internal),
        { "System.Net.Dns::GetHostByName_internal", (void*)Dns_GetHostByName_internal },
        { "System.Net.Dns::GetHostByAddr_internal", (void*)Dns_GetHostByAddr_internal },
        { "System.Net.Dns::GetHostName_internal", (void*)Dns_GetHostName_internal },
        { "System.ComponentModel.Win32Exception::W32ErrorMessage", (void*)W32ErrorMessage },
        { "System.Environment::get_SocketSecurityEnabled", (void*)Env_get_SocketSecurityEnabled },
    };
#undef S
    nr_register_icalls(t, (int)(sizeof t / sizeof t[0]));
}
