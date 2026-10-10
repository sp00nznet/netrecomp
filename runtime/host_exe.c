/* host_exe.c: runs a recompiled console program (the conformance tests).
 * The Unity build uses host_mono.c instead. */
#include "nrrt.h"
#include <shellapi.h>

extern int32_t nr_entry(void* args);

static void report(void* ex)
{
    NrClass* k = ((NrObj*)ex)->klass;
    const NrFieldDesc* f = nr_field(k, "message");
    char* msg = f ? nr_utf8(*(void**)((char*)ex + f->offset)) : 0;
    fprintf(stderr, "Unhandled Exception: %s.%s: %s\n", k->ns, k->name, msg ? msg : "");
    nr_free(msg);
}

static int32_t run(void* args)
{
    void* ex = 0;
    int32_t r = 0;
    __try { r = nr_entry(args); }
    __except (nr_catch(GetExceptionInformation(), 0, &ex)) { report(ex); r = 255; }
    return r;
}

int main(void)
{
    nr_init();
    int n = 0;
    LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &n);
    void* args = nr_new_string_array(n > 0 ? n - 1 : 0);
    for (int i = 1; i < n; i++) ((void**)((NrArray*)args)->vector)[i - 1] = nr_str_utf16((uint16_t*)av[i], (int32_t)wcslen(av[i]));
    LocalFree(av);
    int32_t r = run(args);
    fflush(stdout);
    return r;
}
