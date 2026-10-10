/* Scripted input for an embedding player (opt-in: NR_INPUT=<file>).
 *
 * Drives the game window without touching the real mouse or keyboard, so a
 * run on a hidden or virtual monitor can be played by a test or by hand. The
 * player's imports of GetCursorPos, GetAsyncKeyState and GetKeyState are
 * patched to report the scripted pointer and buttons, and the matching window
 * messages are posted. Commands are read from the file as it grows, one per
 * line (coordinates are client pixels):
 *
 *   move X Y        click X Y       down / up      (left button)
 *   rclick X Y      key VK          wait MS
 *
 * Append lines while the game runs (echo click 180 353 >> input.txt). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void nr_log(const char* fmt, ...);

static volatile LONG in_x, in_y, in_active;
static volatile LONG btn_down[256];   /* by virtual key; VK_LBUTTON = 1, VK_RBUTTON = 2 */
static HWND in_hwnd;

static BOOL (WINAPI* real_GetCursorPos)(POINT*);
static SHORT (WINAPI* real_GetAsyncKeyState)(int);
static SHORT (WINAPI* real_GetKeyState)(int);

static BOOL WINAPI fake_GetCursorPos(POINT* p)
{
    if (!in_active || !in_hwnd) return real_GetCursorPos(p);
    p->x = in_x; p->y = in_y;
    ClientToScreen(in_hwnd, p);
    return TRUE;
}
static SHORT WINAPI fake_GetAsyncKeyState(int vk)
{
    if (in_active && vk >= 0 && vk < 256 && btn_down[vk]) return (SHORT)0x8000;
    return in_active && (vk == VK_LBUTTON || vk == VK_RBUTTON) ? 0 : real_GetAsyncKeyState(vk);
}
static SHORT WINAPI fake_GetKeyState(int vk)
{
    if (in_active && vk >= 0 && vk < 256 && btn_down[vk]) return (SHORT)0x8000;
    return real_GetKeyState(vk);
}

/* Swap one import of the main executable; returns the old target. */
static void* patch_import(const char* dll, const char* fn, void* repl)
{
    char* base = (char*)GetModuleHandleA(0);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++)
    {
        if (_stricmp(base + d->Name, dll)) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA* addrs = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp(((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, fn)) continue;
            DWORD old;
            void* prev = (void*)addrs->u1.Function;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            addrs->u1.Function = (ULONG_PTR)repl;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return 0;
}

static BOOL CALLBACK find_window(HWND h, LPARAM lp)
{
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    *(HWND*)lp = h;
    return FALSE;
}

static void post_mouse(UINT msg, WPARAM w)
{
    PostMessageA(in_hwnd, msg, w, MAKELPARAM(in_x, in_y));
}

static void button(int vk, int down)
{
    btn_down[vk] = down;
    UINT msg = vk == VK_LBUTTON ? (down ? WM_LBUTTONDOWN : WM_LBUTTONUP) : (down ? WM_RBUTTONDOWN : WM_RBUTTONUP);
    post_mouse(msg, down ? (vk == VK_LBUTTON ? MK_LBUTTON : MK_RBUTTON) : 0);
}

static void move(int x, int y)
{
    in_x = x; in_y = y; in_active = 1;
    post_mouse(WM_MOUSEMOVE, btn_down[VK_LBUTTON] ? MK_LBUTTON : 0);
}

static void run_command(char* line)
{
    int x, y, v;
    if (sscanf(line, "move %d %d", &x, &y) == 2) move(x, y);
    else if (sscanf(line, "click %d %d", &x, &y) == 2)
    {
        /* frames in between so per-frame hover, press and release all register */
        move(x, y); Sleep(200);
        button(VK_LBUTTON, 1); Sleep(150);
        button(VK_LBUTTON, 0);
    }
    else if (sscanf(line, "rclick %d %d", &x, &y) == 2) { move(x, y); Sleep(200); button(VK_RBUTTON, 1); Sleep(150); button(VK_RBUTTON, 0); }
    else if (!strncmp(line, "down", 4)) button(VK_LBUTTON, 1);
    else if (!strncmp(line, "up", 2)) button(VK_LBUTTON, 0);
    else if (sscanf(line, "key %i", &v) == 1 && v > 0 && v < 256)
    {
        btn_down[v] = 1; PostMessageA(in_hwnd, WM_KEYDOWN, v, 1); Sleep(120);
        btn_down[v] = 0; PostMessageA(in_hwnd, WM_KEYUP, v, 0xC0000001);
    }
    else if (sscanf(line, "wait %d", &v) == 1) Sleep(v);
    else if (line[0] && line[0] != '#') { nr_log("netrecomp: input: unknown command '%s'\n", line); return; }
    nr_log("netrecomp: input: %s\n", line);
}

static DWORD WINAPI input_thread(void* path)
{
    long pos = 0;
    char line[256];
    for (;;)
    {
        if (!in_hwnd) EnumWindows(find_window, (LPARAM)&in_hwnd);
        FILE* f = in_hwnd ? fopen((const char*)path, "r") : 0;
        if (f)
        {
            fseek(f, pos, SEEK_SET);
            while (fgets(line, sizeof line, f))
            {
                size_t n = strlen(line);
                if (!n || line[n - 1] != '\n') break;   /* partial line: retry later */
                pos += (long)n;
                line[--n] = 0;
                if (n && line[n - 1] == '\r') line[--n] = 0;
                fclose(f); f = 0;
                run_command(line);
                f = fopen((const char*)path, "r");
                if (!f) break;
                fseek(f, pos, SEEK_SET);
            }
            if (f) fclose(f);
        }
        Sleep(100);
    }
}

void nr_input_start(void)
{
    static char path[MAX_PATH];
    if (!GetEnvironmentVariableA("NR_INPUT", path, sizeof path)) return;
    real_GetCursorPos = patch_import("USER32.dll", "GetCursorPos", (void*)fake_GetCursorPos);
    real_GetAsyncKeyState = patch_import("USER32.dll", "GetAsyncKeyState", (void*)fake_GetAsyncKeyState);
    real_GetKeyState = patch_import("USER32.dll", "GetKeyState", (void*)fake_GetKeyState);
    if (!real_GetCursorPos) real_GetCursorPos = GetCursorPos;
    if (!real_GetAsyncKeyState) real_GetAsyncKeyState = GetAsyncKeyState;
    if (!real_GetKeyState) real_GetKeyState = GetKeyState;
    CreateThread(0, 0, input_thread, path, 0, 0);
    nr_log("netrecomp: input: reading commands from %s\n", path);
}
