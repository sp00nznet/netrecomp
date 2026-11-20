/* NR_MUTE=1: mute this process's default audio session (for unattended
 * runs). Engines that open WASAPI streams without a session GUID (FMOD in
 * Unity does) join that session, so muting it once at start-up covers them. */
#define COBJMACROS
#define CINTERFACE
#include <winsock2.h>
#include <windows.h>
#include <string.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")

/* the C headers declare these without defining them */
static const GUID CLSID_MMDevEnum_ = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const GUID IID_MMDevEnum_ = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const GUID IID_SessMgr_ = { 0xbfa971f1, 0x4d5e, 0x40bb, { 0x93, 0x5e, 0x96, 0x70, 0x39, 0xbf, 0xbe, 0xe4 } };

void nr_log(const char* fmt, ...);

void nr_mute_start(void)
{
    if (GetEnvironmentVariableA("NR_MUTE", 0, 0) == 0) return;
    HRESULT init = CoInitializeEx(0, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = 0;
    IMMDevice* dev = 0;
    IAudioSessionManager* mgr = 0;
    ISimpleAudioVolume* vol = 0;
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDevEnum_, 0, CLSCTX_ALL, &IID_MMDevEnum_, (void**)&en))
        && SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))
        && SUCCEEDED(IMMDevice_Activate(dev, &IID_SessMgr_, CLSCTX_ALL, 0, (void**)&mgr))
        && SUCCEEDED(IAudioSessionManager_GetSimpleAudioVolume(mgr, 0, FALSE, &vol)))
        ok = SUCCEEDED(ISimpleAudioVolume_SetMute(vol, TRUE, 0));
    if (vol) ISimpleAudioVolume_Release(vol);
    if (mgr) IAudioSessionManager_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    if (SUCCEEDED(init)) CoUninitialize();
    nr_log("netrecomp: NR_MUTE %s\n", ok ? "on" : "failed (no audio device?)");
}

/* NR_OFFLINE=1: no network for this process. The resolver and connect entry
 * points of ws2_32 are overwritten with a stub that fails, so neither managed
 * code nor the native player (analytics, crash upload, web requests) reaches
 * a server. Loopback is still refused too: ponytail: allow 127.0.0.1 if a
 * game ever needs a local server. */
static int WSAAPI offline_fail(void) { WSASetLastError(WSAENETUNREACH); return SOCKET_ERROR; }
static void* WSAAPI offline_null(void) { WSASetLastError(WSAHOST_NOT_FOUND); return 0; }
static int WSAAPI offline_eai(void) { return WSAHOST_NOT_FOUND; }
static void stub_out(HMODULE m, const char* fn, void* to)
{
    unsigned char* p = m ? (unsigned char*)GetProcAddress(m, fn) : 0;
    DWORD old;
    if (!p || !VirtualProtect(p, 12, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = 0x48; p[1] = 0xB8; memcpy(p + 2, &to, 8);   /* mov rax, to */
    p[10] = 0xFF; p[11] = 0xE0;                       /* jmp rax */
    VirtualProtect(p, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 12);
}
void nr_offline_start(void)
{
    if (GetEnvironmentVariableA("NR_OFFLINE", 0, 0) == 0) return;
    HMODULE w = LoadLibraryA("ws2_32.dll");
    static const char* fail[] = { "connect", "WSAConnect", "WSAConnectByNameA", "WSAConnectByNameW", "sendto", "WSASendTo" };
    static const char* eai[] = { "getaddrinfo", "GetAddrInfoW", "GetAddrInfoExA", "GetAddrInfoExW" };
    for (int i = 0; i < 6; i++) stub_out(w, fail[i], (void*)offline_fail);
    for (int i = 0; i < 4; i++) stub_out(w, eai[i], (void*)offline_eai);
    stub_out(w, "gethostbyname", (void*)offline_null);
    nr_log("netrecomp: NR_OFFLINE on\n");
}
