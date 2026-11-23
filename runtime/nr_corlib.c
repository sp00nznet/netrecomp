/* nr_corlib.c: mono 2.x mscorlib internal calls (objects, strings, arrays,
 * math, environment, time, GC, threads, monitors, file I/O).
 *
 * Signatures follow the managed extern declarations: instance icalls take
 * `this` first, value types by value, ref/out as pointers. Overloaded icalls
 * are registered under mono's "Type::Method(sig)" key. Reflection lives in
 * nr_reflect.c. See docs/runtime.md. */
#include "nrrt.h"
#include <time.h>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#define OBJ(o) ((NrObj*)(o))
#define STR(o) ((NrString*)(o))
#define ARR(o) ((NrArray*)(o))

static int32_t foff(NrClass* k, const char* name)
{
    const NrFieldDesc* f = nr_field(k, name);
    if (!f) { nr_log("netrecomp: field %s.%s::%s missing\n", k->ns, k->name, name); return 0; }
    return f->offset;
}
#define FIELD(o, T, off) (*(T*)((char*)(o) + (off)))

/* ---- System.Object ---- */

static void* Object_GetType(void* self) { return nr_type_object(OBJ(self)->klass); }

static void* Object_MemberwiseClone(void* self)
{
    NrClass* k = OBJ(self)->klass;
    size_t size;
    if (k == nr_k_string) size = offsetof(NrString, chars) + 2 * ((size_t)STR(self)->length + 1);
    else if (k->flags & NR_ARRAY)
    {
        NrArray* a = ARR(self);
        size = offsetof(NrArray, vector) + (size_t)k->elem_size * a->max_length;
        if (a->bounds) size = ((size + 7) & ~7u) + sizeof(NrBounds) * k->rank;
    }
    else size = (size_t)k->instance_size;
    void* o = (k->flags & NR_HAS_REFS) ? GC_MALLOC(size) : GC_MALLOC_ATOMIC(size);
    memcpy(o, self, size);
    OBJ(o)->sync = 0;
    if ((k->flags & NR_ARRAY) && ARR(self)->bounds)
        ARR(o)->bounds = (NrBounds*)((char*)o + ((char*)ARR(self)->bounds - (char*)self));
    return o;
}

/* Addresses don't move (non-moving collector), so the address is a stable hash. */
static int32_t Object_InternalGetHashCode(void* o) { return (int32_t)(((uintptr_t)o >> 3) * 2654435761u); }
static intptr_t Object_obj_address(void* self) { return (intptr_t)self; }

/* ---- System.ValueType ----
 * mono compares the raw bits of reference-free fields here and hands the
 * reference fields back as pairs for managed Equals. */
static int vt_fields_have_refs(NrClass* k)
{
    for (int i = 0; i < k->n_fields; i++)
    {
        const NrFieldDesc* f = &k->fields[i];
        if ((f->flags & 0x10) || f->offset < 0) continue;      /* static */
        if (nr_is_ref(f->type) || (f->type->flags & NR_HAS_REFS)) return 1;
    }
    return 0;
}

static uint8_t ValueType_InternalEquals(void* a, void* b, void** fields)
{
    *fields = 0;
    NrClass* k = OBJ(a)->klass;
    if (OBJ(b)->klass != k) return 0;
    if (!vt_fields_have_refs(k))
        return memcmp((char*)a + sizeof(NrObj), (char*)b + sizeof(NrObj), (size_t)k->value_size) == 0;
    int n = 0;
    for (int i = 0; i < k->n_fields; i++) if (!(k->fields[i].flags & 0x10) && k->fields[i].offset >= 0) n++;
    void** pairs = (void**)nr_newarr(nr_k_objarr, n * 2);
    int j = 0;
    for (int i = 0; i < k->n_fields; i++)
    {
        const NrFieldDesc* f = &k->fields[i];
        if ((f->flags & 0x10) || f->offset < 0) continue;
        void *va, *vb;
        if (nr_is_ref(f->type)) { va = *(void**)((char*)a + f->offset); vb = *(void**)((char*)b + f->offset); }
        else { va = nr_box(f->type, (char*)a + f->offset); vb = nr_box(f->type, (char*)b + f->offset); }
        ((void**)ARR(pairs)->vector)[j++] = va;
        ((void**)ARR(pairs)->vector)[j++] = vb;
    }
    *fields = pairs;
    return 1;
}

static int32_t ValueType_InternalGetHashCode(void* o, void** fields)
{
    NrClass* k = OBJ(o)->klass;
    *fields = 0;
    int32_t h = 0;
    int n = 0;
    for (int i = 0; i < k->n_fields; i++)
    {
        const NrFieldDesc* f = &k->fields[i];
        if ((f->flags & 0x10) || f->offset < 0) continue;
        if (nr_is_ref(f->type)) { n++; continue; }
        const uint8_t* p = (const uint8_t*)o + f->offset;
        for (int b = 0; b < nr_value_size(f->type); b++) h = h * 31 + p[b];
    }
    if (n)
    {
        void* arr = nr_newarr(nr_k_objarr, n);
        int j = 0;
        for (int i = 0; i < k->n_fields; i++)
        {
            const NrFieldDesc* f = &k->fields[i];
            if (!(f->flags & 0x10) && f->offset >= 0 && nr_is_ref(f->type))
                ((void**)ARR(arr)->vector)[j++] = *(void**)((char*)o + f->offset);
        }
        *fields = arr;
    }
    return h;
}

/* ---- System.String ---- */

static void* String_InternalAllocateStr(int32_t len) { return nr_str_alloc(len); }

typedef struct Intern { NrString* s; struct Intern* next; } Intern;
static Intern* intern_tab[4096];
static CRITICAL_SECTION intern_lock;
static uint32_t str_hash(NrString* s)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < s->length; i++) h = (h ^ s->chars[i]) * 16777619u;
    return h & 4095;
}
static NrString* intern_find(NrString* s, int add)
{
    EnterCriticalSection(&intern_lock);
    uint32_t h = str_hash(s);
    for (Intern* e = intern_tab[h]; e; e = e->next)
        if (e->s->length == s->length && !memcmp(e->s->chars, s->chars, 2 * (size_t)s->length)) { LeaveCriticalSection(&intern_lock); return e->s; }
    NrString* r = 0;
    if (add)
    {
        Intern* e = (Intern*)GC_MALLOC_UNCOLLECTABLE(sizeof *e);
        e->s = s; e->next = intern_tab[h]; intern_tab[h] = e;
        r = s;
    }
    LeaveCriticalSection(&intern_lock);
    return r;
}
static void* String_InternalIntern(void* s) { return intern_find(STR(s), 1); }
static void* String_InternalIsInterned(void* s) { return intern_find(STR(s), 0); }

/* InternalSplit(char[] separator, int count, int options): options bit 1 = RemoveEmptyEntries */
static int is_sep(uint16_t c, NrArray* seps)
{
    if (!seps || seps->max_length == 0) return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xa0 || c == 0x2028 || c == 0x2029 || c == 0x3000 || (c >= 0x2000 && c <= 0x200b) || c == 0x1680 || c == 0xfeff;
    for (uint32_t i = 0; i < seps->max_length; i++) if (((uint16_t*)seps->vector)[i] == c) return 1;
    return 0;
}
static void* String_InternalSplit(void* self, void* seps, int32_t count, int32_t options)
{
    NrString* s = STR(self);
    int remove_empty = options & 1;
    int n = 0, start = 0;
    /* scanned memory: the parts are only reachable from here while later ones allocate */
    void** parts = (void**)GC_MALLOC(sizeof(void*) * ((size_t)s->length + 1));
    for (int i = 0; i <= s->length && n < count; i++)
    {
        int last = n == count - 1;
        if (i == s->length || (!last && is_sep(s->chars[i], ARR(seps))))
        {
            if (last && i < s->length) i = s->length;
            int len = i - start;
            if (len > 0 || !remove_empty) parts[n++] = nr_str_utf16(s->chars + start, len);
            start = i + 1;
            if (remove_empty && n == count - 1)
            {
                while (start < s->length && is_sep(s->chars[start], ARR(seps))) start++;
                if (start < s->length) { parts[n++] = nr_str_utf16(s->chars + start, s->length - start); }
                break;
            }
        }
    }
    void* arr = nr_new_string_array(n);
    for (int i = 0; i < n; i++) ((void**)ARR(arr)->vector)[i] = parts[i];
    GC_FREE(parts);
    return arr;
}

/* ---- System.Array ---- */

static int32_t Array_GetRank(void* self) { return OBJ(self)->klass->rank; }
static int32_t Array_GetLength(void* self, int32_t dim)
{
    NrArray* a = ARR(self);
    if (dim < 0 || dim >= OBJ(self)->klass->rank) nr_throw_range();
    return a->bounds ? (int32_t)a->bounds[dim].length : (int32_t)a->max_length;
}
static int32_t Array_GetLowerBound(void* self, int32_t dim)
{
    NrArray* a = ARR(self);
    if (dim < 0 || dim >= OBJ(self)->klass->rank) nr_throw_range();
    return a->bounds ? a->bounds[dim].lower_bound : 0;
}
static void Array_ClearInternal(void* a, int32_t idx, int32_t len)
{
    int32_t es = OBJ(a)->klass->elem_size;
    memset((char*)ARR(a)->vector + (size_t)idx * es, 0, (size_t)len * es);
}
static void* Array_Clone(void* self) { return Object_MemberwiseClone(self); }

static uint8_t Array_FastCopy(void* src, int32_t si, void* dst, int32_t di, int32_t len)
{
    NrClass *sk = OBJ(src)->klass, *dk = OBJ(dst)->klass;
    if (sk->rank != 1 || dk->rank != 1) return 0;
    if (si < 0 || di < 0 || len < 0) return 0;
    if ((uint32_t)si + (uint32_t)len > ARR(src)->max_length || (uint32_t)di + (uint32_t)len > ARR(dst)->max_length) return 0;
    NrClass *se = sk->elem, *de = dk->elem;
    if (se != de)
    {
        /* ref -> ref is fine when every source element fits the destination */
        if (!(nr_is_ref(se) && nr_is_ref(de) && nr_assignable(de, se))) return 0;
    }
    memmove((char*)ARR(dst)->vector + (size_t)di * dk->elem_size, (char*)ARR(src)->vector + (size_t)si * sk->elem_size, (size_t)len * sk->elem_size);
    return 1;
}

static void* elem_box(void* arr, int32_t idx)
{
    NrClass* k = OBJ(arr)->klass;
    void* p = (char*)ARR(arr)->vector + (size_t)idx * k->elem_size;
    return nr_is_ref(k->elem) ? *(void**)p : nr_box(k->elem, p);
}
static void elem_set(void* arr, int32_t idx, void* v)
{
    NrClass* k = OBJ(arr)->klass;
    void* p = (char*)ARR(arr)->vector + (size_t)idx * k->elem_size;
    if (nr_is_ref(k->elem))
    {
        if (v && !nr_isinst(v, k->elem)) nr_throw_new("System", "InvalidCastException", "Object cannot be stored in an array of this type.");
        *(void**)p = v;
    }
    else if (!v) memset(p, 0, (size_t)k->elem_size);
    else
    {
        NrClass* vk = OBJ(v)->klass;
        NrClass* e = k->elem;
        NrClass* ue = (e->flags & NR_ENUM) ? e->elem : e;
        NrClass* uv = (vk->flags & NR_ENUM) ? vk->elem : vk;
        if (ue != uv) nr_throw_new("System", "InvalidCastException", "Object cannot be stored in an array of this type.");
        memcpy(p, (char*)v + sizeof(NrObj), (size_t)k->elem_size);
    }
}
static void* Array_GetValueImpl(void* self, int32_t pos) { return elem_box(self, pos); }
static void Array_SetValueImpl(void* self, void* v, int32_t pos) { elem_set(self, pos, v); }
static int32_t md_index(void* self, void* idx)
{
    NrArray* a = ARR(self);
    NrArray* ia = ARR(idx);
    int rank = OBJ(self)->klass->rank;
    if ((int)ia->max_length != rank) nr_throw_new("System", "ArgumentException", "Indices length does not match the array rank.");
    if (!a->bounds) { int32_t i = ((int32_t*)ia->vector)[0]; if ((uint32_t)i >= a->max_length) nr_throw_range(); return i; }
    return (int32_t)(((char*)nr_mdarray_addr(self, rank, (int32_t*)ia->vector) - (char*)a->vector) / OBJ(self)->klass->elem_size);
}
static void* Array_GetValue(void* self, void* idx) { return elem_box(self, md_index(self, idx)); }
static void Array_SetValue(void* self, void* v, void* idx) { elem_set(self, md_index(self, idx), v); }
static void Array_GetGenericValueImpl(void* self, int32_t pos, void* out, NrClass* t)
{
    NrClass* k = OBJ(self)->klass;
    (void)t;
    memcpy(out, (char*)ARR(self)->vector + (size_t)pos * k->elem_size, (size_t)k->elem_size);
}
static void Array_SetGenericValueImpl(void* self, int32_t pos, void* in, NrClass* t)
{
    NrClass* k = OBJ(self)->klass;
    (void)t;
    memcpy((char*)ARR(self)->vector + (size_t)pos * k->elem_size, in, (size_t)k->elem_size);
}
static void* Array_CreateInstanceImpl(void* type, void* lengths, void* bounds)
{
    NrClass* e = nr_type_class(type);
    int rank = (int)ARR(lengths)->max_length;
    int32_t* lens = (int32_t*)ARR(lengths)->vector;
    if (rank == 1 && (!bounds || ((int32_t*)ARR(bounds)->vector)[0] == 0))
        return nr_newarr(nr_array_class(e, 1), lens[0]);
    void* a = nr_new_mdarray(nr_array_class(e, rank), rank, lens);
    if (bounds) for (int i = 0; i < rank; i++) ARR(a)->bounds[i].lower_bound = ((int32_t*)ARR(bounds)->vector)[i];
    return a;
}

/* ---- System.Buffer ---- */

static int32_t Buffer_ByteLengthInternal(void* a)
{
    NrClass* k = OBJ(a)->klass;
    if (!(k->flags & NR_ARRAY) || nr_is_ref(k->elem) || (k->elem->flags & NR_HAS_REFS) || !(k->elem->byval_arg.type >= 2 && k->elem->byval_arg.type <= 0x0d || k->elem->byval_arg.type == 0x18 || k->elem->byval_arg.type == 0x19))
        return -1;
    return (int32_t)(ARR(a)->max_length * (uint32_t)k->elem_size);
}
static uint8_t Buffer_BlockCopyInternal(void* src, int32_t so, void* dst, int32_t d0, int32_t count)
{
    if (count < 0 || so < 0 || d0 < 0) return 0;
    int32_t sl = Buffer_ByteLengthInternal(src), dl = Buffer_ByteLengthInternal(dst);
    if (so > sl - count || d0 > dl - count) return 0;
    memmove((char*)ARR(dst)->vector + d0, (char*)ARR(src)->vector + so, (size_t)count);
    return 1;
}
static uint8_t Buffer_GetByteInternal(void* a, int32_t i) { return ((uint8_t*)ARR(a)->vector)[i]; }
static void Buffer_SetByteInternal(void* a, int32_t i, int32_t v) { ((uint8_t*)ARR(a)->vector)[i] = (uint8_t)v; }

/* ---- RuntimeHelpers ---- */

static void RuntimeHelpers_InitializeArray(void* arr, intptr_t field)
{
    const NrFieldDesc* f = (const NrFieldDesc*)field;
    int32_t n = (int32_t)(ARR(arr)->max_length * (uint32_t)OBJ(arr)->klass->elem_size);
    if (!f || !f->rva || f->rva_size < n) nr_throw_new("System", "ArgumentException", "Field does not hold initial data for this array.");
    memcpy(ARR(arr)->vector, f->rva, (size_t)n);
}
static int32_t RuntimeHelpers_get_OffsetToStringData(void) { return (int32_t)offsetof(NrString, chars); }
static void* RuntimeHelpers_GetObjectValue(void* o)
{
    if (o && (OBJ(o)->klass->flags & NR_VALUETYPE)) return Object_MemberwiseClone(o);
    return o;
}
static void RuntimeHelpers_RunClassConstructor(intptr_t handle)
{
    NrType* t = (NrType*)handle;
    if (t && t->klass && t->klass->cctor_state != 2) nr_run_cctor(t->klass);
}

/* ---- System.Math ---- */

static double Math_Floor(double d) { return floor(d); }
static double Math_Ceiling(double d) { return ceil(d); }
static double Math_Round(double d)
{
    double f = floor(d), r = d - f;
    if (r > 0.5 || (r == 0.5 && fmod(f, 2.0) != 0)) f += 1.0;
    return f;
}
static double Math_Round2(double v, int32_t digits, uint8_t away)
{
    double p = pow(10.0, digits), x = v * p;
    double f = floor(x), r = x - f;
    if (away) { if (r >= 0.5) f += 1.0; }
    else if (r > 0.5 || (r == 0.5 && fmod(f, 2.0) != 0)) f += 1.0;
    return f / p;
}
static double Math_Sqrt(double d) { return sqrt(d); }
static double Math_Pow(double a, double b) { return pow(a, b); }
static double Math_Sin(double d) { return sin(d); }
static double Math_Cos(double d) { return cos(d); }
static double Math_Tan(double d) { return tan(d); }
static double Math_Asin(double d) { return asin(d); }
static double Math_Acos(double d) { return acos(d); }
static double Math_Atan(double d) { return atan(d); }
static double Math_Atan2(double a, double b) { return atan2(a, b); }
static double Math_Sinh(double d) { return sinh(d); }
static double Math_Cosh(double d) { return cosh(d); }
static double Math_Tanh(double d) { return tanh(d); }
static double Math_Exp(double d) { return exp(d); }
static double Math_Log(double d) { return log(d); }
static double Math_Log10(double d) { return log10(d); }
static double Math_Abs_d(double d) { return fabs(d); }
static float Math_Abs_f(float d) { return fabsf(d); }

/* ---- System.Environment ---- */

static void* Environment_get_NewLine(void) { return nr_str_utf8("\r\n"); }
static int32_t Environment_get_Platform(void) { return 2; /* Win32NT */ }
static int32_t Environment_get_TickCount(void) { return (int32_t)GetTickCount(); }
static int32_t Environment_get_ProcessorCount(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int32_t)si.dwNumberOfProcessors; }
static void* Environment_internalGetEnvironmentVariable(void* name)
{
    if (!name) return 0;
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW((LPCWSTR)STR(name)->chars, buf, 32768);
    if (!n || n >= 32768) return 0;
    return nr_str_utf16((uint16_t*)buf, (int32_t)n);
}
static void* Environment_GetMachineConfigPath(void) { return nr_str_utf8("machine.config"); }
static void* Environment_get_MachineName(void)
{
    wchar_t buf[256]; DWORD n = 256;
    if (!GetComputerNameW(buf, &n)) return nr_str_utf8("localhost");
    return nr_str_utf16((uint16_t*)buf, (int32_t)n);
}
static void* Environment_get_UserName(void) { return Environment_internalGetEnvironmentVariable(nr_str_utf8("USERNAME")); }
static void* Environment_GetOSVersionString(void) { return nr_str_utf8("6.1.7601.65536"); }
static uint8_t Environment_get_HasShutdownStarted(void) { return 0; }
static void Environment_Exit(int32_t code) { ExitProcess((UINT)code); }
static int32_t Environment_get_ExitCode(void) { return 0; }
static void Environment_set_ExitCode(int32_t c) { (void)c; }
static void* Environment_GetCommandLineArgs(void)
{
    int n = 0;
    LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &n);
    void* arr = nr_new_string_array(n);
    for (int i = 0; i < n; i++) ((void**)ARR(arr)->vector)[i] = nr_str_utf16((uint16_t*)av[i], (int32_t)wcslen(av[i]));
    LocalFree(av);
    return arr;
}
static void* Environment_GetEnvironmentVariableNames(void)
{
    LPWCH env = GetEnvironmentStringsW();
    int n = 0;
    for (LPWCH p = env; *p; p += wcslen(p) + 1) if (*p != '=') n++;
    void* arr = nr_new_string_array(n);
    int i = 0;
    for (LPWCH p = env; *p; p += wcslen(p) + 1)
    {
        if (*p == '=') continue;
        LPWCH eq = wcschr(p, '=');
        ((void**)ARR(arr)->vector)[i++] = nr_str_utf16((uint16_t*)p, (int32_t)(eq ? eq - p : (int32_t)wcslen(p)));
    }
    FreeEnvironmentStringsW(env);
    return arr;
}
static void Environment_InternalSetEnvironmentVariable(void* name, void* value)
{
    SetEnvironmentVariableW((LPCWSTR)STR(name)->chars, value ? (LPCWSTR)STR(value)->chars : 0);
}
static void* Environment_GetWindowsFolderPath(int32_t folder)
{
    wchar_t buf[MAX_PATH];
    /* NR_DATA_DIR also stands in for the per-user folders a game writes to (Documents,
     * AppData, LocalAppData, Saved Games...: <NR_DATA_DIR>/folder-<CSIDL>), so a test run
     * can't touch the player's real (possibly cloud-synced) files. */
    static const int32_t user[] = { 0x05 /*Personal*/, 0x1a /*AppData*/, 0x1c /*LocalAppData*/, 0x0d /*MyMusic*/, 0x27 /*MyPictures*/, 0x0e /*MyVideo*/, 0x10 /*Desktop*/ };
    wchar_t dd[MAX_PATH];
    if (GetEnvironmentVariableW(L"NR_DATA_DIR", dd, MAX_PATH))
        for (int i = 0; i < 7; i++)
            if ((folder & 0xff) == user[i])
            {
                _snwprintf(buf, MAX_PATH, L"%s\\folder-%02x", dd, user[i]);
                buf[MAX_PATH - 1] = 0;
                CreateDirectoryW(dd, 0); CreateDirectoryW(buf, 0);
                return nr_str_utf16((uint16_t*)buf, (int32_t)wcslen(buf));
            }
    if (SHGetFolderPathW(0, folder, 0, 0, buf) != 0) return nr_str_utf8("");
    return nr_str_utf16((uint16_t*)buf, (int32_t)wcslen(buf));
}
static void* Environment_GetLogicalDrivesInternal(void)
{
    DWORD m = GetLogicalDrives();
    int n = 0;
    for (int i = 0; i < 26; i++) if (m & (1u << i)) n++;
    void* arr = nr_new_string_array(n);
    int j = 0;
    for (int i = 0; i < 26; i++)
        if (m & (1u << i)) { char d[4] = { (char)('A' + i), ':', '\\', 0 }; ((void**)ARR(arr)->vector)[j++] = nr_str_utf8(d); }
    return arr;
}
static void Environment_internalBroadcastSettingChange(void) {}

/* ---- time ---- */

#define TICKS_1601 504911232000000000LL
static int64_t DateTime_GetNow(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) + TICKS_1601;
}
static int64_t DateTime_GetTimeMonotonic(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 10000000.0 / (double)f.QuadPart);
}
static int64_t Stopwatch_GetTimestamp(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }

/* CurrentSystemTimeZone.GetTimeZoneData(int year, out long[] data, out string[] names):
 * data = { daylight start, daylight end, utc offset, daylight delta } in ticks. */
static uint8_t TimeZone_GetTimeZoneData(int32_t year, void** data, void** names)
{
    TIME_ZONE_INFORMATION tz;
    DWORD r = GetTimeZoneInformation(&tz);
    NrClass* lk = nr_array_class(nr_k_int64, 1);
    void* d = nr_newarr(lk, 4);
    int64_t* v = (int64_t*)ARR(d)->vector;
    v[2] = -(int64_t)tz.Bias * 600000000LL;
    v[3] = 0; v[0] = 0; v[1] = 0;
    if (r == TIME_ZONE_ID_DAYLIGHT || (r == TIME_ZONE_ID_STANDARD && tz.DaylightDate.wMonth))
    {
        v[3] = -(int64_t)tz.DaylightBias * 600000000LL;
        SYSTEMTIME s = tz.DaylightDate, e = tz.StandardDate;
        /* wYear 0 means "day-of-week rule"; resolve it for the year */
        SYSTEMTIME* t[2] = { &s, &e };
        for (int i = 0; i < 2; i++)
        {
            SYSTEMTIME x = *t[i];
            if (x.wYear == 0)
            {
                SYSTEMTIME first = { (WORD)year, x.wMonth, 0, 1, 0, 0, 0, 0 };
                FILETIME ft; SystemTimeToFileTime(&first, &ft); FileTimeToSystemTime(&ft, &first);
                int day = 1 + (x.wDayOfWeek - first.wDayOfWeek + 7) % 7 + (x.wDay - 1) * 7;
                int dim[] = { 31, (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
                while (day > dim[x.wMonth - 1]) day -= 7;
                x.wYear = (WORD)year; x.wDay = (WORD)day;
            }
            FILETIME ft; SystemTimeToFileTime(&x, &ft);
            v[i] = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) + TICKS_1601;
        }
    }
    void* n = nr_new_string_array(2);
    ((void**)ARR(n)->vector)[0] = nr_str_utf16((uint16_t*)tz.StandardName, (int32_t)wcslen(tz.StandardName));
    ((void**)ARR(n)->vector)[1] = nr_str_utf16((uint16_t*)tz.DaylightName, (int32_t)wcslen(tz.DaylightName));
    *data = d; *names = n;
    return 1;
}

/* ---- System.GC ---- */

static void GC_SuppressFinalize(void* o) { if (o) GC_REGISTER_FINALIZER_NO_ORDER(o, 0, 0, 0, 0); }
static void GC_ReRegisterForFinalize(void* o) { (void)o; }
static void GC_InternalCollect(int32_t gen) { (void)gen; GC_gcollect(); }
static int64_t GC_GetTotalMemory(uint8_t force) { if (force) GC_gcollect(); return (int64_t)GC_get_heap_size() - (int64_t)GC_get_free_bytes(); }
static int32_t GC_get_MaxGeneration(void) { return 0; }
static void GC_KeepAlive(void* o) { (void)o; }
static void GC_WaitForPendingFinalizers(void) { GC_invoke_finalizers(); }
static int32_t GC_CollectionCount(int32_t g) { (void)g; return (int32_t)GC_get_gc_no(); }

/* ---- GCHandle ---- */

static int32_t GCHandle_GetTargetHandle(void* o, int32_t h, int32_t type)
{
    if (h) { /* retarget: free and reallocate keeps it simple */ nr_gchandle_free((uint32_t)h); }
    if (type == 0 || type == 1) return (int32_t)nr_gchandle_new_weak(o);
    return (int32_t)nr_gchandle_new(o, type == 3);
}
static void* GCHandle_GetTarget(int32_t h) { return nr_gchandle_target((uint32_t)h); }
static void GCHandle_FreeHandle(int32_t h) { nr_gchandle_free((uint32_t)h); }
static intptr_t GCHandle_GetAddrOfPinnedObject(int32_t h)
{
    void* o = nr_gchandle_target((uint32_t)h);
    if (!o) return 0;
    if (OBJ(o)->klass == nr_k_string) return (intptr_t)STR(o)->chars;
    if (OBJ(o)->klass->flags & NR_ARRAY) return (intptr_t)ARR(o)->vector;
    return (intptr_t)((char*)o + sizeof(NrObj));
}
static uint8_t GCHandle_CheckCurrentDomain(int32_t h) { (void)h; return 1; }

/* ---- Marshal (the common subset) ---- */

/* ponytail: Marshal.SizeOf ignores [MarshalAs(ByValArray/ByValTStr)] layouts,
 * so a buffer sized by it can be smaller than what native code writes into it
 * (Steamworks callbacks). The slack keeps that off the heap; the real fix is
 * marshal sizes and copies computed from the MarshalAs metadata. */
static intptr_t Marshal_AllocHGlobal(intptr_t n) { void* p = calloc(1, (size_t)n + 4096); if (!p) nr_throw_new("System", "OutOfMemoryException", 0); return (intptr_t)p; }
static void Marshal_FreeHGlobal(intptr_t p) { free((void*)p); }
static intptr_t Marshal_AllocCoTaskMem(int32_t n) { return (intptr_t)CoTaskMemAlloc((SIZE_T)n); }
static void Marshal_FreeCoTaskMem(intptr_t p) { CoTaskMemFree((void*)p); }
static uint8_t Marshal_ReadByte(intptr_t p, int32_t o) { return *(uint8_t*)(p + o); }
static int16_t Marshal_ReadInt16(intptr_t p, int32_t o) { int16_t v; memcpy(&v, (void*)(p + o), 2); return v; }
static int32_t Marshal_ReadInt32(intptr_t p, int32_t o) { int32_t v; memcpy(&v, (void*)(p + o), 4); return v; }
static int64_t Marshal_ReadInt64(intptr_t p, int32_t o) { int64_t v; memcpy(&v, (void*)(p + o), 8); return v; }
static intptr_t Marshal_ReadIntPtr(intptr_t p, int32_t o) { intptr_t v; memcpy(&v, (void*)(p + o), sizeof v); return v; }
static void Marshal_WriteByte(intptr_t p, int32_t o, uint8_t v) { *(uint8_t*)(p + o) = v; }
static void Marshal_WriteInt16(intptr_t p, int32_t o, int16_t v) { memcpy((void*)(p + o), &v, 2); }
static void Marshal_WriteInt32(intptr_t p, int32_t o, int32_t v) { memcpy((void*)(p + o), &v, 4); }
static void Marshal_WriteInt64(intptr_t p, int32_t o, int64_t v) { memcpy((void*)(p + o), &v, 8); }
static void Marshal_WriteIntPtr(intptr_t p, int32_t o, intptr_t v) { memcpy((void*)(p + o), &v, sizeof v); }
static void Marshal_copy_to_unmanaged(void* src, int32_t si, intptr_t dst, int32_t len)
{
    int32_t es = OBJ(src)->klass->elem_size;
    memcpy((void*)dst, (char*)ARR(src)->vector + (size_t)si * es, (size_t)len * es);
}
static void Marshal_copy_from_unmanaged(intptr_t src, int32_t di, void* dst, int32_t len)
{
    int32_t es = OBJ(dst)->klass->elem_size;
    memcpy((char*)ARR(dst)->vector + (size_t)di * es, (void*)src, (size_t)len * es);
}
static void* Marshal_PtrToStringAnsi(intptr_t p) { return p ? nr_str_utf8((const char*)p) : 0; }
static void* Marshal_PtrToStringAnsi2(intptr_t p, int32_t len)
{
    if (!p) return 0;
    char* t = (char*)malloc((size_t)len + 1); memcpy(t, (void*)p, (size_t)len); t[len] = 0;
    void* s = nr_str_utf8(t); free(t); return s;
}
static void* Marshal_PtrToStringUni(intptr_t p) { return p ? nr_str_from_utf16z((const uint16_t*)p) : 0; }
static void* Marshal_PtrToStringUni2(intptr_t p, int32_t len) { return p ? nr_str_utf16((const uint16_t*)p, len) : 0; }
static intptr_t Marshal_StringToHGlobalAnsi(void* s) { return (intptr_t)(s ? nr_utf8(s) : 0); }
static intptr_t Marshal_StringToHGlobalUni(void* s)
{
    if (!s) return 0;
    size_t n = 2 * ((size_t)STR(s)->length + 1);
    uint16_t* p = (uint16_t*)malloc(n);
    memcpy(p, STR(s)->chars, n);
    return (intptr_t)p;
}
static int32_t Marshal_GetLastWin32Error(void) { return (int32_t)GetLastError(); }
static int32_t Marshal_SizeOf(void* t)
{
    NrClass* k = nr_type_class(t);
    for (int i = 0; i < nr_n_marshal_sizes; i++) if (nr_marshal_sizes[i].klass == k) return nr_marshal_sizes[i].size;
    return nr_value_size(k);
}
static intptr_t Marshal_UnsafeAddrOfPinnedArrayElement(void* arr, int32_t idx) { return (intptr_t)((char*)ARR(arr)->vector + (size_t)idx * OBJ(arr)->klass->elem_size); }
/* Delegate fields marshal as native function pointers (Steamworks.NET builds
 * its callback vtables this way). ponytail: other non-blittable fields
 * (bool, string, MarshalAs arrays) still copy as their managed bits. */
static void marshal_delegates(NrClass* k, const char* src, char* dst)
{
    for (; k; k = k->parent)
        for (int i = 0; i < k->n_fields; i++)
        {
            const NrFieldDesc* f = &k->fields[i];
            if ((f->flags & 0x10) || f->offset < 0 || !(f->type->flags & NR_DELEGATE)) continue;
            void* d = *(void* const*)(src + f->offset);
            *(void**)(dst + f->offset - sizeof(NrObj)) = d ? nr_delegate_to_fnptr(d) : 0;
        }
}
static void Marshal_StructureToPtr(void* o, intptr_t p, uint8_t del)
{
    (void)del;
    NrClass* k = OBJ(o)->klass;
    memcpy((void*)p, (char*)o + sizeof(NrObj), (size_t)(k->flags & NR_VALUETYPE ? k->value_size : k->instance_size - (int32_t)sizeof(NrObj)));
    marshal_delegates(k, (const char*)o, (char*)p);
}
static void* Marshal_PtrToStructure(intptr_t p, void* t)
{
    NrClass* k = nr_type_class(t);
    void* o = nr_new(k);
    memcpy((char*)o + sizeof(NrObj), (void*)p, (size_t)(k->flags & NR_VALUETYPE ? k->value_size : k->instance_size - (int32_t)sizeof(NrObj)));
    return o;
}
static void Marshal_PtrToStructureObject(intptr_t p, void* o)
{
    NrClass* k = OBJ(o)->klass;
    memcpy((char*)o + sizeof(NrObj), (void*)p, (size_t)(k->instance_size - (int32_t)sizeof(NrObj)));
}

/* ---- threads ---- */

typedef struct NrThreadInfo { void* thread_obj; uint32_t handle; } NrThreadInfo;
static __declspec(thread) NrThreadInfo* tinfo;
static int32_t off_thread_state = -1, off_thread_id, off_start_obj, off_threadstart, off_cached_culture, off_cached_ui, off_name, off_handle;

#if !NR_CORLIB4
static void thread_offsets(void)
{
    if (off_thread_state >= 0) return;
    off_thread_id = foff(nr_k_thread, "thread_id");
    off_start_obj = foff(nr_k_thread, "start_obj");
    off_threadstart = foff(nr_k_thread, "threadstart");
    off_cached_culture = foff(nr_k_thread, "cached_culture_info");
    off_name = foff(nr_k_thread, "name");
    off_handle = foff(nr_k_thread, "system_thread_handle");
    off_thread_state = foff(nr_k_thread, "state");
}
#endif

#if NR_CORLIB4
/* mono 4.x: Thread keeps its state in an InternalThread (thread_id, state,
 * name, handle, managed_id); the offsets below are InternalThread's. */
static NrClass* k_ithread;
static int32_t off_internal, off_m_delegate, off_m_arg, off_managed_id;
static volatile LONG next_managed_id;
static void thread_offsets(void)
{
    if (off_thread_state >= 0) return;
    k_ithread = nr_class_by_name("System.Threading", "InternalThread");
    off_internal = foff(nr_k_thread, "internal_thread");
    off_m_delegate = foff(nr_k_thread, "m_Delegate");
    off_m_arg = foff(nr_k_thread, "m_ThreadStartArg");
    off_thread_id = foff(k_ithread, "thread_id");
    off_name = foff(k_ithread, "name");
    off_handle = foff(k_ithread, "handle");
    off_managed_id = foff(k_ithread, "managed_id");
    off_thread_state = foff(k_ithread, "state");
}
static void* internal_of(void* t)
{
    thread_offsets();
    void* it = FIELD(t, void*, off_internal);
    if (!it)
    {
        it = nr_new(k_ithread);
        FIELD(it, int32_t, off_managed_id) = (int32_t)InterlockedIncrement(&next_managed_id);
        FIELD(it, int32_t, off_thread_state) = 8;   /* Unstarted */
        FIELD(t, void*, off_internal) = it;
    }
    return it;
}
#endif

static void* thread_object_for_current(void* existing)
{
    thread_offsets();
    void* t = existing;
    if (!t) t = nr_new(nr_k_thread);
#if NR_CORLIB4
    void* it = internal_of(t);
    FIELD(it, int32_t, off_thread_state) = 0;   /* Running */
    FIELD(it, int64_t, off_thread_id) = (int64_t)GetCurrentThreadId();
#else
    FIELD(t, int32_t, off_thread_state) = 0;   /* Running */
    FIELD(t, int64_t, off_thread_id) = (int64_t)GetCurrentThreadId();
#endif
    NrThreadInfo* ti = (NrThreadInfo*)calloc(1, sizeof *ti);
    ti->thread_obj = t;
    ti->handle = nr_gchandle_new(t, 0);
    tinfo = ti;
    return t;
}

void nr_thread_attach(void)
{
    if (tinfo) return;
    struct GC_stack_base sb;
    if (GC_get_stack_base(&sb) == GC_SUCCESS) GC_register_my_thread(&sb);
    thread_object_for_current(0);
}

/* an engine thread leaving managed code for good (mono_thread_detach) */
void nr_thread_detach(void)
{
    if (!tinfo) return;
    nr_gchandle_free(tinfo->handle);
    free(tinfo); tinfo = 0;
    GC_unregister_my_thread();
}

void* nr_current_thread_object(void)
{
    if (!tinfo) nr_thread_attach();
    return tinfo->thread_obj;
}

static void* Thread_CurrentThread_internal(void) { return nr_current_thread_object(); }
static void Thread_Thread_init(void* self) { (void)self; thread_offsets(); }
static void* Thread_GetCachedCurrentCulture(void* self) { thread_offsets(); return FIELD(self, void*, off_cached_culture); }
static void Thread_SetCachedCurrentCulture(void* self, void* c) { thread_offsets(); FIELD(self, void*, off_cached_culture) = c; }
static void* Thread_GetNull(void* self) { (void)self; return 0; }
static void Thread_SetIgnore(void* self, void* v) { (void)self; (void)v; }
static int32_t Thread_GetDomainID(void) { return 1; }
/* No [MarshalAs] metadata is kept: report none (XmlSerializer asks for every field). */
static void* FieldInfo_GetUnmanagedMarshal(void* self) { (void)self; return 0; }
/* No machine.config is bundled: System.Configuration then reads the file from disk, or none. */
static void* InternalConfigurationHost_get_bundled_machine_config(void) { return 0; }
/* No version resources are read: FileVersionInfo keeps its empty defaults. */
static void FileVersionInfo_GetVersionInfo_internal(void* self, void* file) { (void)self; (void)file; }
/* Unity 4.6's corlib numbers managed threads itself (mono 2.6: ids start at 1) */
static int32_t Thread_GetNewManagedId_internal(void) { static volatile LONG id; return InterlockedIncrement(&id); }
static void Thread_Thread_free_internal(void* self, intptr_t h) { (void)self; if (h) CloseHandle((HANDLE)h); }
static void Thread_Sleep_internal(int32_t ms) { Sleep(ms < 0 ? INFINITE : (DWORD)ms); }
static void Thread_MemoryBarrier(void) { MemoryBarrier(); }
static void Thread_SpinWait_nop(void) { YieldProcessor(); }
static void* Thread_GetName_internal(void* self) { thread_offsets(); void* n = FIELD(self, void*, off_name); return n; }
static void Thread_SetName_internal(void* self, void* n) { thread_offsets(); FIELD(self, void*, off_name) = n; }
static int32_t Thread_GetState(void* self) { thread_offsets(); return FIELD(self, int32_t, off_thread_state); }
static void Thread_SetState(void* self, int32_t s) { thread_offsets(); FIELD(self, int32_t, off_thread_state) |= s; }
static void Thread_ClrState(void* self, int32_t s) { thread_offsets(); FIELD(self, int32_t, off_thread_state) &= ~s; }
static void Thread_ResetAbort_internal(void) {}
static void Thread_FreeLocalSlotValues(int32_t slot, uint8_t tl) { (void)slot; (void)tl; }

typedef struct StartArgs { void* thread; uint32_t h; HANDLE ready; uint32_t dh, ah; } StartArgs;

static DWORD WINAPI thread_main(LPVOID p)
{
    /* CreateThread is GC_CreateThread here (gc.h redirects it under
     * GC_THREADS), so the collector already knows this thread. */
    StartArgs* a = (StartArgs*)p;
    void* t = nr_gchandle_target(a->h);
    nr_gchandle_free(a->h);
#if NR_CORLIB4
    /* the delegate and argument were captured at start: Start clears m_ThreadStartArg.
     * Read them before signalling: *a lives on the starting thread's stack. */
    void* d = nr_gchandle_target(a->dh);
    void* arg = a->ah ? nr_gchandle_target(a->ah) : 0;
    nr_gchandle_free(a->dh); if (a->ah) nr_gchandle_free(a->ah);
    SetEvent(a->ready);
    thread_object_for_current(t);
#else
    SetEvent(a->ready);
    thread_object_for_current(t);
    void* d = FIELD(t, void*, off_threadstart);
    void* arg = FIELD(t, void*, off_start_obj);
#endif
    const NrMethodDesc* inv = nr_method(OBJ(d)->klass, "Invoke", -1);
    void* ex = 0;
    void* args[1] = { arg };
    nr_invoke(inv, d, inv->n_params ? args : 0, &ex);
    if (ex)
    {
        NrClass* k = OBJ(ex)->klass;
        if (strcmp(k->name, "ThreadAbortException"))
        {
            nr_log("netrecomp: unhandled exception %s.%s on thread %lu\n", k->ns, k->name, GetCurrentThreadId());
            if (nr_unhandled_hook) nr_unhandled_hook(ex);
        }
    }
#if NR_CORLIB4
    FIELD(internal_of(t), int32_t, off_thread_state) = 16;  /* Stopped */
#else
    FIELD(t, int32_t, off_thread_state) = 16;  /* Stopped */
#endif
    nr_gchandle_free(tinfo->handle);
    free(tinfo); tinfo = 0;
    return 0;
}

static intptr_t Thread_Thread_internal(void* self, void* start)
{
    thread_offsets();
    (void)start;
    StartArgs a = { self, nr_gchandle_new(self, 0), CreateEventA(0, TRUE, FALSE, 0), 0, 0 };
#if NR_CORLIB4
    a.dh = nr_gchandle_new(start ? start : FIELD(self, void*, off_m_delegate), 0);
    void* sarg = FIELD(self, void*, off_m_arg);
    a.ah = sarg ? nr_gchandle_new(sarg, 0) : 0;
    void* it = internal_of(self);
#endif
    HANDLE h = CreateThread(0, 0, thread_main, &a, 0, 0);
    if (!h) { CloseHandle(a.ready); return 0; }
    WaitForSingleObject(a.ready, INFINITE);
    CloseHandle(a.ready);
#if NR_CORLIB4
    FIELD(it, intptr_t, off_handle) = (intptr_t)h;
#else
    FIELD(self, intptr_t, off_handle) = (intptr_t)h;
#endif
    return (intptr_t)h;
}

static uint8_t Thread_Join_internal(void* self, int32_t ms, intptr_t h)
{
    (void)self;
    return WaitForSingleObject((HANDLE)h, ms < 0 ? INFINITE : (DWORD)ms) == WAIT_OBJECT_0;
}

static void Thread_Abort_internal(void* self, void* state) { (void)self; (void)state; nr_throw_notsupported("Thread.Abort"); }
static void Thread_Interrupt_internal(void* self) { (void)self; }

/* volatile access */
static int32_t VR_i4(int32_t* p) { MemoryBarrier(); return *(volatile int32_t*)p; }
static int64_t VR_i8(int64_t* p) { return InterlockedCompareExchange64((volatile LONG64*)p, 0, 0); }
static double VR_r8(double* p) { MemoryBarrier(); return *(volatile double*)p; }
static float VR_r4(float* p) { MemoryBarrier(); return *(volatile float*)p; }
static int32_t VR_u1(uint8_t* p) { MemoryBarrier(); return *(volatile uint8_t*)p; }
static int32_t VR_i2(int16_t* p) { MemoryBarrier(); return *(volatile int16_t*)p; }
static intptr_t VR_ip(intptr_t* p) { MemoryBarrier(); return *(volatile intptr_t*)p; }
static void VW_ip(intptr_t* p, intptr_t v) { *(volatile intptr_t*)p = v; MemoryBarrier(); }
static void* VR_ref(void** p) { MemoryBarrier(); return *(void* volatile*)p; }
static void VW_i4(int32_t* p, int32_t v) { *(volatile int32_t*)p = v; MemoryBarrier(); }
static void VW_i8(int64_t* p, int64_t v) { InterlockedExchange64((volatile LONG64*)p, v); }
static void VW_r8(double* p, double v) { *(volatile double*)p = v; MemoryBarrier(); }
static void VW_r4(float* p, float v) { *(volatile float*)p = v; MemoryBarrier(); }
static void VW_u1(uint8_t* p, int32_t v) { *(volatile uint8_t*)p = (uint8_t)v; MemoryBarrier(); }
static void VW_i2(int16_t* p, int32_t v) { *(volatile int16_t*)p = (int16_t)v; MemoryBarrier(); }
static void VW_ref(void** p, void* v) { *(void* volatile*)p = v; MemoryBarrier(); }

/* ---- Interlocked ---- */

static int32_t IL_CmpXchg_i4(int32_t* p, int32_t v, int32_t c) { return InterlockedCompareExchange((volatile LONG*)p, v, c); }
static void* IL_CmpXchg_ref(void** p, void* v, void* c) { return InterlockedCompareExchangePointer(p, v, c); }
static void* IL_CmpXchg_T(void** p, void* v, void* c, NrClass* t) { (void)t; return InterlockedCompareExchangePointer(p, v, c); }
static float IL_CmpXchg_r4(float* p, float v, float c) { LONG r = InterlockedCompareExchange((volatile LONG*)p, *(LONG*)&v, *(LONG*)&c); return *(float*)&r; }
static int64_t IL_CmpXchg_i8(int64_t* p, int64_t v, int64_t c) { return InterlockedCompareExchange64((volatile LONG64*)p, v, c); }
static intptr_t IL_CmpXchg_ip(intptr_t* p, intptr_t v, intptr_t c) { return (intptr_t)InterlockedCompareExchangePointer((void**)p, (void*)v, (void*)c); }
static double IL_CmpXchg_r8(double* p, double v, double c) { LONG64 r = InterlockedCompareExchange64((volatile LONG64*)p, *(LONG64*)&v, *(LONG64*)&c); return *(double*)&r; }
static int32_t IL_Xchg_i4(int32_t* p, int32_t v) { return InterlockedExchange((volatile LONG*)p, v); }
static void* IL_Xchg_ref(void** p, void* v) { return InterlockedExchangePointer(p, v); }
static void* IL_Xchg_T(void** p, void* v, NrClass* t) { (void)t; return InterlockedExchangePointer(p, v); }
static float IL_Xchg_r4(float* p, float v) { LONG r = InterlockedExchange((volatile LONG*)p, *(LONG*)&v); return *(float*)&r; }
static int64_t IL_Xchg_i8(int64_t* p, int64_t v) { return InterlockedExchange64((volatile LONG64*)p, v); }
static intptr_t IL_Xchg_ip(intptr_t* p, intptr_t v) { return (intptr_t)InterlockedExchangePointer((void**)p, (void*)v); }
static double IL_Xchg_r8(double* p, double v) { LONG64 r = InterlockedExchange64((volatile LONG64*)p, *(LONG64*)&v); return *(double*)&r; }
static int32_t IL_Inc_i4(int32_t* p) { return InterlockedIncrement((volatile LONG*)p); }
static int64_t IL_Inc_i8(int64_t* p) { return InterlockedIncrement64((volatile LONG64*)p); }
static int32_t IL_Dec_i4(int32_t* p) { return InterlockedDecrement((volatile LONG*)p); }
static int64_t IL_Dec_i8(int64_t* p) { return InterlockedDecrement64((volatile LONG64*)p); }
static int32_t IL_Add_i4(int32_t* p, int32_t v) { return InterlockedExchangeAdd((volatile LONG*)p, v) + v; }
static int64_t IL_Add_i8(int64_t* p, int64_t v) { return InterlockedExchangeAdd64((volatile LONG64*)p, v) + v; }
static int64_t IL_Read_i8(int64_t* p) { return InterlockedCompareExchange64((volatile LONG64*)p, 0, 0); }

/* ---- Monitor ----
 * An object's sync word points at a lazily created lock. ponytail: locks
 * are never freed; reclaim them from a finalizer if long runs leak. */
typedef struct NrMon { CRITICAL_SECTION cs; CONDITION_VARIABLE cv; DWORD owner; int32_t count; } NrMon;

int nr_debug(void);
static NrMon* mon_of(void* o)
{
    if (!o) nr_throw_new("System", "ArgumentNullException", "obj");
    NrMon* m = (NrMon*)OBJ(o)->sync;
    if (m) return m;
    NrMon* n = (NrMon*)calloc(1, sizeof *n);
    InitializeCriticalSectionAndSpinCount(&n->cs, 2000);
    InitializeConditionVariable(&n->cv);
    m = (NrMon*)InterlockedCompareExchangePointer(&OBJ(o)->sync, n, 0);
    if (m) { DeleteCriticalSection(&n->cs); free(n); return m; }
    return n;
}

static void mon_enter(NrMon* m) { EnterCriticalSection(&m->cs); m->owner = GetCurrentThreadId(); m->count++; }
static void Monitor_Enter(void* o) { mon_enter(mon_of(o)); }
static uint8_t Monitor_try_enter(void* o, int32_t ms)
{
    NrMon* m = mon_of(o);
    if (ms < 0) { mon_enter(m); return 1; }
    DWORD start = GetTickCount();
    for (;;)
    {
        if (TryEnterCriticalSection(&m->cs)) { m->owner = GetCurrentThreadId(); m->count++; return 1; }
        if ((int32_t)(GetTickCount() - start) >= ms) return 0;
        Sleep(1);
    }
}
static void Monitor_Exit(void* o)
{
    NrMon* m = mon_of(o);
    if (m->owner != GetCurrentThreadId())
    {
        if (nr_debug()) nr_log("netrecomp: Monitor.Exit on %s.%s %p: owner %lu, this thread %lu, count %d\n", OBJ(o)->klass->ns, OBJ(o)->klass->name, o, m->owner, GetCurrentThreadId(), m->count);
        nr_throw_new("System.Threading", "SynchronizationLockException", "Object synchronization method was called from an unsynchronized block of code.");
    }
    if (--m->count == 0) m->owner = 0;
    LeaveCriticalSection(&m->cs);
}
static uint8_t Monitor_test_owner(void* o) { NrMon* m = (NrMon*)OBJ(o)->sync; return m && m->owner == GetCurrentThreadId(); }
static uint8_t Monitor_test_synchronised(void* o) { NrMon* m = (NrMon*)OBJ(o)->sync; return m && m->owner != 0; }
static void Monitor_pulse(void* o) { WakeConditionVariable(&mon_of(o)->cv); }
static void Monitor_pulse_all(void* o) { WakeAllConditionVariable(&mon_of(o)->cv); }
static uint8_t Monitor_wait(void* o, int32_t ms)
{
    NrMon* m = mon_of(o);
    if (m->owner != GetCurrentThreadId()) nr_throw_new("System.Threading", "SynchronizationLockException", "Object is not synchronized");
    int32_t n = m->count;
    for (int i = 1; i < n; i++) LeaveCriticalSection(&m->cs);
    m->count = 0; m->owner = 0;
    BOOL ok = SleepConditionVariableCS(&m->cv, &m->cs, ms < 0 ? INFINITE : (DWORD)ms);
    for (int i = 1; i < n; i++) EnterCriticalSection(&m->cs);
    m->count = n; m->owner = GetCurrentThreadId();
    return ok ? 1 : 0;
}

/* ---- wait handles and events ---- */

static intptr_t Event_Create(uint8_t manual, uint8_t initial, void* name, uint8_t* created)
{
    HANDLE h = CreateEventW(0, manual, initial, name ? (LPCWSTR)STR(name)->chars : 0);
    *created = GetLastError() != ERROR_ALREADY_EXISTS;
    return (intptr_t)h;
}
static uint8_t Event_Set(intptr_t h) { return SetEvent((HANDLE)h) != 0; }
static uint8_t Event_Reset(intptr_t h) { return ResetEvent((HANDLE)h) != 0; }
static void Event_Close(intptr_t h) { CloseHandle((HANDLE)h); }
static uint8_t WaitHandle_WaitOne_internal(void* self, intptr_t h, int32_t ms, uint8_t exit_ctx)
{
    (void)self; (void)exit_ctx;
    return WaitForSingleObject((HANDLE)h, ms < 0 ? INFINITE : (DWORD)ms) == WAIT_OBJECT_0;
}
static int32_t wait_many(void* handles, int32_t ms, BOOL all)
{
    NrArray* a = ARR(handles);
    HANDLE hs[64];
    int n = (int)a->max_length;
    if (n > 64) nr_throw_notsupported("waiting on more than 64 handles");
    const NrFieldDesc* f = 0;
    for (int i = 0; i < n; i++)
    {
        void* wh = ((void**)a->vector)[i];
        if (!f) f = nr_field(OBJ(wh)->klass, "os_handle");
        hs[i] = f ? *(HANDLE*)((char*)wh + f->offset) : 0;
        if (!hs[i])
        {
            /* 2.0 WaitHandle keeps a SafeWaitHandle; its handle field is "handle" */
            const NrFieldDesc* sf = nr_field(OBJ(wh)->klass, "safe_wait_handle");
            if (sf) { void* swh = *(void**)((char*)wh + sf->offset); const NrFieldDesc* hf = nr_field(OBJ(swh)->klass, "handle"); hs[i] = *(HANDLE*)((char*)swh + hf->offset); }
        }
    }
    DWORD r = WaitForMultipleObjects((DWORD)n, hs, all, ms < 0 ? INFINITE : (DWORD)ms);
    if (r == WAIT_TIMEOUT) return 258;
    return (int32_t)(r - WAIT_OBJECT_0);
}
static int32_t WaitHandle_WaitAny_internal(void* handles, int32_t ms, uint8_t ctx) { (void)ctx; return wait_many(handles, ms, FALSE); }
static uint8_t WaitHandle_WaitAll_internal(void* handles, int32_t ms, uint8_t ctx) { (void)ctx; return wait_many(handles, ms, TRUE) != 258; }
static uint8_t WaitHandle_SignalAndWait_Internal(intptr_t s, intptr_t w, int32_t ms) { return SignalObjectAndWait((HANDLE)s, (HANDLE)w, ms < 0 ? INFINITE : (DWORD)ms, FALSE) == WAIT_OBJECT_0; }
static intptr_t Mutex_Create(uint8_t owned, void* name, uint8_t* created)
{
    HANDLE h = CreateMutexW(0, owned, name ? (LPCWSTR)STR(name)->chars : 0);
    *created = GetLastError() != ERROR_ALREADY_EXISTS;
    return (intptr_t)h;
}
static uint8_t Mutex_Release(intptr_t h) { return ReleaseMutex((HANDLE)h) != 0; }
static intptr_t Semaphore_Create(int32_t init, int32_t max, void* name, uint8_t* created)
{
    HANDLE h = CreateSemaphoreW(0, init, max, name ? (LPCWSTR)STR(name)->chars : 0);
    *created = GetLastError() != ERROR_ALREADY_EXISTS;
    return (intptr_t)h;
}
static int32_t Semaphore_Release(intptr_t h, int32_t n, uint8_t* fail)
{
    LONG prev = 0;
    *fail = !ReleaseSemaphore((HANDLE)h, n, &prev);
    return (int32_t)prev;
}

/* ---- System.IO.MonoIO ---- */

static int32_t io_err(void) { return (int32_t)GetLastError(); }
static const wchar_t* W(void* s) { return s ? (const wchar_t*)STR(s)->chars : L""; }

static intptr_t MonoIO_Open(void* path, int32_t mode, int32_t access, int32_t share, int32_t options, int32_t* err)
{
    DWORD acc = 0, disp;
    if (access & 1) acc |= GENERIC_READ;
    if (access & 2) acc |= GENERIC_WRITE;
    switch (mode)
    {
    case 1: disp = CREATE_NEW; break;
    case 2: disp = CREATE_ALWAYS; break;
    case 3: disp = OPEN_EXISTING; break;
    case 4: disp = OPEN_ALWAYS; break;
    case 5: disp = TRUNCATE_EXISTING; break;
    case 6: disp = OPEN_ALWAYS; acc |= FILE_APPEND_DATA; break;
    default: disp = OPEN_EXISTING;
    }
    DWORD sh = 0;
    if (share & 1) sh |= FILE_SHARE_READ;
    if (share & 2) sh |= FILE_SHARE_WRITE;
    if (share & 4) sh |= FILE_SHARE_DELETE;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (options & 0x04000000) flags |= FILE_FLAG_DELETE_ON_CLOSE;
    if (options & 0x10000000) flags |= FILE_FLAG_RANDOM_ACCESS;
    if (options & 0x08000000) flags |= FILE_FLAG_SEQUENTIAL_SCAN;
    if (options & 0x80000000) flags |= FILE_FLAG_WRITE_THROUGH;
    HANDLE h = CreateFileW(W(path), acc, sh, 0, disp, flags, 0);
    if (h == INVALID_HANDLE_VALUE) { *err = io_err(); return -1; }
    *err = 0;
    return (intptr_t)h;
}
static uint8_t MonoIO_Close(intptr_t h, int32_t* err) { BOOL ok = CloseHandle((HANDLE)h); *err = ok ? 0 : io_err(); return ok != 0; }
static int32_t MonoIO_Read(intptr_t h, void* buf, int32_t off, int32_t count, int32_t* err)
{
    DWORD n = 0;
    BOOL ok = ReadFile((HANDLE)h, (char*)ARR(buf)->vector + off, (DWORD)count, &n, 0);
    if (!ok)
    {
        DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE) { *err = 0; return 0; }
        *err = (int32_t)e; return -1;
    }
    *err = 0;
    return (int32_t)n;
}
static int32_t MonoIO_Write(intptr_t h, void* buf, int32_t off, int32_t count, int32_t* err)
{
    DWORD n = 0;
    BOOL ok = WriteFile((HANDLE)h, (char*)ARR(buf)->vector + off, (DWORD)count, &n, 0);
    *err = ok ? 0 : io_err();
    return ok ? (int32_t)n : -1;
}
static int64_t MonoIO_Seek(intptr_t h, int64_t off, int32_t origin, int32_t* err)
{
    LARGE_INTEGER d, r; d.QuadPart = off;
    BOOL ok = SetFilePointerEx((HANDLE)h, d, &r, (DWORD)origin);
    *err = ok ? 0 : io_err();
    return ok ? r.QuadPart : -1;
}
static uint8_t MonoIO_Flush(intptr_t h, int32_t* err) { BOOL ok = FlushFileBuffers((HANDLE)h); *err = ok ? 0 : io_err(); if (!ok && *err == ERROR_INVALID_HANDLE) { *err = 0; ok = 1; } return ok != 0; }
static int64_t MonoIO_GetLength(intptr_t h, int32_t* err)
{
    LARGE_INTEGER s;
    BOOL ok = GetFileSizeEx((HANDLE)h, &s);
    *err = ok ? 0 : io_err();
    return ok ? s.QuadPart : -1;
}
static uint8_t MonoIO_SetLength(intptr_t h, int64_t len, int32_t* err)
{
    LARGE_INTEGER cur, z = { 0 }, d;
    SetFilePointerEx((HANDLE)h, z, &cur, FILE_CURRENT);
    d.QuadPart = len;
    BOOL ok = SetFilePointerEx((HANDLE)h, d, 0, FILE_BEGIN) && SetEndOfFile((HANDLE)h);
    SetFilePointerEx((HANDLE)h, cur, 0, FILE_BEGIN);
    *err = ok ? 0 : io_err();
    return ok != 0;
}
static int32_t MonoIO_GetFileType(intptr_t h, int32_t* err) { *err = 0; return (int32_t)GetFileType((HANDLE)h); }
static int32_t MonoIO_GetFileAttributes(void* path, int32_t* err)
{
    DWORD a = GetFileAttributesW(W(path));
    *err = a == INVALID_FILE_ATTRIBUTES ? io_err() : 0;
    return (int32_t)a;
}
static uint8_t MonoIO_SetFileAttributes(void* path, int32_t attrs, int32_t* err) { BOOL ok = SetFileAttributesW(W(path), (DWORD)attrs); *err = ok ? 0 : io_err(); return ok != 0; }

typedef struct MonoIOStat { void* name; int32_t attrs; int64_t length, ctime, atime, mtime; } MonoIOStat;
static int64_t ft_ticks(FILETIME f) { return (int64_t)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) + TICKS_1601; }
static uint8_t MonoIO_GetFileStat(void* path, MonoIOStat* st, int32_t* err)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(W(path), GetFileExInfoStandard, &d)) { *err = io_err(); memset(st, 0, sizeof *st); return 0; }
    st->name = 0;
    st->attrs = (int32_t)d.dwFileAttributes;
    st->length = (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    st->ctime = ft_ticks(d.ftCreationTime); st->atime = ft_ticks(d.ftLastAccessTime); st->mtime = ft_ticks(d.ftLastWriteTime);
    *err = 0;
    return 1;
}
static uint8_t MonoIO_CreateDirectory(void* path, int32_t* err) { BOOL ok = CreateDirectoryW(W(path), 0); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_RemoveDirectory(void* path, int32_t* err) { BOOL ok = RemoveDirectoryW(W(path)); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_DeleteFile(void* path, int32_t* err) { BOOL ok = DeleteFileW(W(path)); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_MoveFile(void* a, void* b, int32_t* err) { BOOL ok = MoveFileW(W(a), W(b)); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_CopyFile(void* a, void* b, uint8_t ow, int32_t* err) { BOOL ok = CopyFileW(W(a), W(b), !ow); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_ReplaceFile(void* src, void* dst, void* bak, uint8_t ign, int32_t* err)
{
    (void)ign;
    BOOL ok = ReplaceFileW(W(dst), W(src), bak ? W(bak) : 0, 0, 0, 0);
    *err = ok ? 0 : io_err();
    return ok != 0;
}
static void* MonoIO_GetCurrentDirectory(int32_t* err)
{
    wchar_t buf[32768];
    DWORD n = GetCurrentDirectoryW(32768, buf);
    *err = n ? 0 : io_err();
    return nr_str_utf16((uint16_t*)buf, (int32_t)n);
}
static uint8_t MonoIO_SetCurrentDirectory(void* path, int32_t* err) { BOOL ok = SetCurrentDirectoryW(W(path)); *err = ok ? 0 : io_err(); return ok != 0; }
static void* MonoIO_GetFileSystemEntries(void* path, void* pattern, int32_t attrs, int32_t mask, int32_t* err)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(pattern), &fd);
    *err = 0;
    if (h == INVALID_HANDLE_VALUE)
    {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND) *err = (int32_t)e;
        return nr_new_string_array(0);
    }
    int cap = 64, n = 0;
    void** items = (void**)GC_MALLOC(sizeof(void*) * cap);
    int plen = STR(path)->length;
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (((int32_t)fd.dwFileAttributes & mask) != attrs) continue;
        int fl = (int)wcslen(fd.cFileName);
        int sep = plen > 0 && STR(path)->chars[plen - 1] != '\\' && STR(path)->chars[plen - 1] != '/';
        NrString* s = nr_str_alloc(plen + sep + fl);
        memcpy(s->chars, STR(path)->chars, 2 * (size_t)plen);
        if (sep) s->chars[plen] = '\\';
        memcpy(s->chars + plen + sep, fd.cFileName, 2 * (size_t)fl);
        if (n == cap) { void** ni = (void**)GC_MALLOC(sizeof(void*) * cap * 2); memcpy(ni, items, sizeof(void*) * cap); items = ni; cap *= 2; }
        items[n++] = s;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    void* arr = nr_new_string_array(n);
    memcpy(ARR(arr)->vector, items, sizeof(void*) * n);
    return arr;
}
static intptr_t MonoIO_get_ConsoleOutput(void) { return (intptr_t)GetStdHandle(STD_OUTPUT_HANDLE); }
static intptr_t MonoIO_get_ConsoleInput(void) { return (intptr_t)GetStdHandle(STD_INPUT_HANDLE); }
static intptr_t MonoIO_get_ConsoleError(void) { return (intptr_t)GetStdHandle(STD_ERROR_HANDLE); }
static uint16_t MonoIO_get_VolumeSeparatorChar(void) { return ':'; }
static uint16_t MonoIO_get_DirectorySeparatorChar(void) { return '\\'; }
static uint16_t MonoIO_get_AltDirectorySeparatorChar(void) { return '/'; }
static uint16_t MonoIO_get_PathSeparator(void) { return ';'; }
static void* MonoIO_get_InvalidPathChars(void)
{
    static const uint16_t bad[] = { '"', '<', '>', '|', 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    void* a = nr_newarr(nr_array_class(nr_k_char, 1), (int32_t)(sizeof bad / 2));
    memcpy(ARR(a)->vector, bad, sizeof bad);
    return a;
}
static void MonoIO_Lock(intptr_t h, int64_t pos, int64_t len, int32_t* err)
{
    BOOL ok = LockFile((HANDLE)h, (DWORD)pos, (DWORD)(pos >> 32), (DWORD)len, (DWORD)(len >> 32));
    *err = ok ? 0 : io_err();
}
static void MonoIO_Unlock(intptr_t h, int64_t pos, int64_t len, int32_t* err)
{
    BOOL ok = UnlockFile((HANDLE)h, (DWORD)pos, (DWORD)(pos >> 32), (DWORD)len, (DWORD)(len >> 32));
    *err = ok ? 0 : io_err();
}
static uint8_t MonoIO_SetFileTime(intptr_t h, int64_t c, int64_t a, int64_t w, int32_t* err)
{
    FILETIME fc, fa, fw;
    int64_t v[3] = { c, a, w };
    FILETIME* f[3] = { &fc, &fa, &fw };
    for (int i = 0; i < 3; i++) if (v[i] >= 0) { uint64_t t = (uint64_t)(v[i] - TICKS_1601); f[i]->dwLowDateTime = (DWORD)t; f[i]->dwHighDateTime = (DWORD)(t >> 32); }
    BOOL ok = SetFileTime((HANDLE)h, c >= 0 ? &fc : 0, a >= 0 ? &fa : 0, w >= 0 ? &fw : 0);
    *err = ok ? 0 : io_err();
    return ok != 0;
}
static int32_t Path_GetTempPath_len(void) { return 0; }
static void* Path_get_temp_path(void)
{
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return nr_str_utf16((uint16_t*)buf, (int32_t)n);
}

/* ---- RNGCryptoServiceProvider: the system RNG, no per-instance state ---- */

static uint8_t Rng_Open(void) { return 1; }
static intptr_t Rng_Initialize(void* seed) { (void)seed; return 1; }
static intptr_t Rng_GetBytes(intptr_t h, void* data)
{
    NrArray* a = ARR(data);
    if (a->max_length && BCryptGenRandom(0, (PUCHAR)a->vector, a->max_length, BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        nr_throw_new("System.Security.Cryptography", "CryptographicException", "BCryptGenRandom failed");
    return h;
}
static void Rng_Close(intptr_t h) { (void)h; }

/* ---- Encoding / culture ---- */

static void* Encoding_InternalCodePage(int32_t* cp)
{
    /* An unknown name makes Encoding.Default fall back to UTF8Unmarked (UTF-8,
     * no BOM). ponytail: mono on Windows reported the ANSI code page, which
     * needs the I18N assemblies by reflection; report it if a title reads
     * text through Encoding.Default and gets the wrong characters. */
    *cp = -1;
    return nr_str_utf8("netrecomp-default");
}
static uint8_t CultureInfo_construct_from_current_locale(void* self, void* ci) { (void)self; (void)ci; return 0; }
static uint8_t CultureInfo_construct_internal_locale_from_current_locale(void* ci) { (void)ci; return 0; }
static uint8_t CultureInfo_false1(void* a) { (void)a; return 0; }
static uint8_t CultureInfo_false2(void* a, int32_t b) { (void)a; (void)b; return 0; }
static uint8_t CultureInfo_false2s(void* a, void* b) { (void)a; (void)b; return 0; }
static void CultureInfo_void1(void* a) { (void)a; }
/* A named culture ("en-US") is built as the invariant culture under that name: same
 * number/date/text data, Name/TwoLetterISOLanguageName from the request. Games ask
 * for en-US to get invariant-style parsing, which this gives. ponytail: no per-culture
 * data (no ICU / culture tables); add them if a title formats for display in a locale. */
static uint8_t CultureInfo_from_name(void* self, void* name)
{
    NrClass* k = OBJ(self)->klass;
    void* inv = 0;
    for (int i = 0; i < k->n_methods && !inv; i++)
        if (!strcmp(k->methods[i].name, "get_InvariantCulture") && k->methods[i].fn) inv = ((void* (*)(void))k->methods[i].fn)();
    NrString* n = (NrString*)name;
    if (!inv || !n || n->length < 2 || n->length > 40) return 0;
    memcpy((char*)self + sizeof(NrObj), (char*)inv + sizeof(NrObj), (size_t)k->instance_size - sizeof(NrObj));
    static const char* lazy[] = { "numInfo", "dateTimeInfo", "textInfo", "compareInfo", "calendar", "parent_culture", "cached_serialized_form", "m_cultureData" };
    for (int i = 0; i < 8; i++) { int32_t o = foff(k, lazy[i]); if (o > 0) *(void**)((char*)self + o) = 0; }
    uint16_t c[41]; int32_t len = n->length, dash = -1;
    for (int32_t i = 0; i < len; i++) { c[i] = n->chars[i]; if (c[i] == '-' && dash < 0) dash = i; }
    for (int32_t i = dash + 1; dash > 0 && i < len && len - dash - 1 == 2; i++) if (c[i] >= 'a' && c[i] <= 'z') c[i] -= 32;
    int32_t o;
    if ((o = foff(k, "m_name")) > 0) *(void**)((char*)self + o) = nr_str_utf16(c, len);
    if ((o = foff(k, "iso2lang")) > 0) *(void**)((char*)self + o) = nr_str_utf16(c, 2);
    if ((o = foff(k, "m_isReadOnly")) > 0) *((uint8_t*)self + o) = 0;
    /* TextInfoData: ansi, ebcdic, mac, oem code pages, right_to_left, list_sep */
    static const struct { int32_t ansi, ebcdic, mac, oem; uint8_t rtl, list_sep; } tid = { 1252, 37, 10000, 437, 0, ',' };
    if ((o = foff(k, "textinfo_data")) > 0) *(const void**)((char*)self + o) = &tid;
    return 1;
}

/* CompareInfo without ICU: ordinal, or case-insensitive ordinal. */
#define IGNORE_CASE(opts) (((opts) & 1) || ((opts) & 0x10000000))   /* IgnoreCase, OrdinalIgnoreCase */
static uint16_t up(uint16_t c) { return (uint16_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)c); }
static int32_t CompareInfo_internal_compare(void* self, void* s1, int32_t o1, int32_t l1, void* s2, int32_t o2, int32_t l2, int32_t opts)
{
    (void)self;
    const uint16_t* a = STR(s1)->chars + o1;
    const uint16_t* b = STR(s2)->chars + o2;
    int ic = IGNORE_CASE(opts);
    if (!(opts & 0x40000000) && !(opts & 0x10000000))
    {
        int r = CompareStringW(LOCALE_INVARIANT, ic ? NORM_IGNORECASE : 0, (LPCWCH)a, l1, (LPCWCH)b, l2);
        if (r) return r - 2;
    }
    int n = l1 < l2 ? l1 : l2;
    for (int i = 0; i < n; i++)
    {
        uint16_t x = ic ? up(a[i]) : a[i], y = ic ? up(b[i]) : b[i];
        if (x != y) return x < y ? -1 : 1;
    }
    return l1 == l2 ? 0 : (l1 < l2 ? -1 : 1);
}
static int32_t CompareInfo_internal_index_char(void* self, void* s, int32_t start, int32_t count, uint16_t c, int32_t opts, uint8_t first)
{
    (void)self;
    const uint16_t* p = STR(s)->chars;
    int ic = IGNORE_CASE(opts);
    uint16_t cc = ic ? up(c) : c;
    if (first) { for (int i = start; i < start + count; i++) if ((ic ? up(p[i]) : p[i]) == cc) return i; }
    else { for (int i = start; i > start - count; i--) if ((ic ? up(p[i]) : p[i]) == cc) return i; }
    return -1;
}
static int32_t CompareInfo_internal_index_str(void* self, void* s, int32_t start, int32_t count, void* v, int32_t opts, uint8_t first)
{
    (void)self;
    const uint16_t* p = STR(s)->chars;
    const uint16_t* q = STR(v)->chars;
    int m = STR(v)->length, ic = IGNORE_CASE(opts);
    if (m == 0) return first ? start : start;
    if (first)
    {
        for (int i = start; i + m <= start + count; i++)
        {
            int j = 0;
            for (; j < m; j++) if ((ic ? up(p[i + j]) : p[i + j]) != (ic ? up(q[j]) : q[j])) break;
            if (j == m) return i;
        }
    }
    else
    {
        for (int i = start - m + 1; i >= start - count + 1 && i >= 0; i--)
        {
            int j = 0;
            for (; j < m; j++) if ((ic ? up(p[i + j]) : p[i + j]) != (ic ? up(q[j]) : q[j])) break;
            if (j == m) return i;
        }
    }
    return -1;
}
static void CompareInfo_construct_compareinfo(void* self, void* locale) { (void)self; (void)locale; }
static void CompareInfo_free_internal_collator(void* self) { (void)self; }
static void CompareInfo_assign_sortkey(void* self, void* key, void* src, int32_t opts)
{
    /* ordinal key, consistent with internal_compare: UTF-16 big endian, upper-cased
     * under IgnoreCase (1), so byte order is code-point order */
    (void)self;
    NrString* str = (NrString*)src;
    int n = str ? str->length : 0;
    void* bytes = nr_newarr(nr_array_class(nr_k_byte, 1), n * 2);
    uint8_t* b = (uint8_t*)((NrArray*)bytes)->vector;
    for (int i = 0; i < n; i++)
    {
        uint16_t c = str->chars[i];
        if ((opts & 1) && c >= 'a' && c <= 'z') c -= 32;
        b[2 * i] = (uint8_t)(c >> 8); b[2 * i + 1] = (uint8_t)c;
    }
    NrClass* k = ((NrObj*)key)->klass;
    const NrFieldDesc* fk = nr_field(k, "key");
    const NrFieldDesc* fs = nr_field(k, "source");
    if (fk) *(void**)((char*)key + fk->offset) = bytes;
    if (fs) *(void**)((char*)key + fs->offset) = src;
}

/* Char tables come from the generated chartab.c (Unicode data, not mono's). */
extern const uint8_t nr_char_category[65536];
extern const uint8_t nr_char_numeric[65536];
extern const double nr_char_numeric_values[];
extern const uint16_t nr_to_lower_low[], nr_to_lower_high[], nr_to_upper_low[], nr_to_upper_high[];
static void Char_GetDataTablePointers(const uint8_t** cat, const uint8_t** num, const double** numv,
                                      const uint16_t** ll, const uint16_t** lh, const uint16_t** ul, const uint16_t** uh)
{
    *cat = nr_char_category; *num = nr_char_numeric; *numv = nr_char_numeric_values;
    *ll = nr_to_lower_low; *lh = nr_to_lower_high; *ul = nr_to_upper_low; *uh = nr_to_upper_high;
}

/* ---- Double / Decimal parsing helpers ---- */

static uint8_t Double_ParseImpl(const char* p, double* r)
{
    char* end;
    *r = strtod(p, &end);
    return end != p;
}

/* ---- misc ---- */

static uint8_t SecurityManager_get_SecurityEnabled(void) { return 0; }
static int32_t NumberFormatter_dummy;

/* ---- AppDomain (single domain) ---- */

static void* the_domain;
static void* AppDomain_getCurDomain(void)
{
    if (!the_domain)
    {
        NrClass* k = nr_class_by_name("System", "AppDomain");
        the_domain = nr_new(k);
        nr_gchandle_new(the_domain, 0);
    }
    return the_domain;
}
static void* AppDomain_getFriendlyName(void* self) { (void)self; return nr_str_utf8("Unity Root Domain"); }
static void* AppDomain_GetData(void* self, void* name) { (void)self; (void)name; return 0; }
static void AppDomain_SetData(void* self, void* name, void* v) { (void)self; (void)name; (void)v; }
static void* AppDomain_getSetup(void* self)
{
    (void)self;
    static void* setup;
    if (!setup) { setup = nr_new(nr_class_by_name("System", "AppDomainSetup")); nr_gchandle_new(setup, 0); }
    return setup;
}
static void* AppDomain_InternalGetContext(void) { return 0; }
static void* AppDomain_InternalSetContext(void* c) { return c; }
static int32_t AppDomain_getDomainID(void* self) { (void)self; return 1; }

/* ---- registration ---- */

extern void nr_register_reflect_icalls(void);
extern void nr_register_net_icalls(void);
extern void nr_register_decimal_icalls(void);
void nr_register_corlib_icalls2(void);
#if NR_CORLIB4
static void nr_register_corlib4_icalls(void);
#endif

static const NrIcall corlib[] = {
    { "System.Object::GetType", Object_GetType },
    { "System.Object::MemberwiseClone", Object_MemberwiseClone },
    { "System.Object::InternalGetHashCode", Object_InternalGetHashCode },
    { "System.Object::obj_address", Object_obj_address },
    { "System.ValueType::InternalEquals", ValueType_InternalEquals },
    { "System.ValueType::InternalGetHashCode", ValueType_InternalGetHashCode },
    { "System.String::InternalAllocateStr", String_InternalAllocateStr },
    { "System.String::InternalIntern", String_InternalIntern },
    { "System.String::InternalIsInterned", String_InternalIsInterned },
    { "System.String::InternalSplit", String_InternalSplit },
    { "System.Array::GetRank", Array_GetRank },
    { "System.Array::GetLength", Array_GetLength },
    { "System.Array::GetLowerBound", Array_GetLowerBound },
    { "System.Array::ClearInternal", Array_ClearInternal },
    { "System.Array::Clone", Array_Clone },
    { "System.Array::FastCopy", Array_FastCopy },
    { "System.Array::GetValueImpl", Array_GetValueImpl },
    { "System.Array::SetValueImpl", Array_SetValueImpl },
    { "System.Array::GetValue", Array_GetValue },
    { "System.Array::SetValue", Array_SetValue },
    { "System.Array::GetGenericValueImpl", Array_GetGenericValueImpl },
    { "System.Array::SetGenericValueImpl", Array_SetGenericValueImpl },
    { "System.Array::CreateInstanceImpl", Array_CreateInstanceImpl },
    { "System.Buffer::ByteLengthInternal", Buffer_ByteLengthInternal },
    { "System.Buffer::BlockCopyInternal", Buffer_BlockCopyInternal },
    { "System.Buffer::GetByteInternal", Buffer_GetByteInternal },
    { "System.Buffer::SetByteInternal", Buffer_SetByteInternal },
    { "System.Runtime.CompilerServices.RuntimeHelpers::InitializeArray", RuntimeHelpers_InitializeArray },
    { "System.Runtime.CompilerServices.RuntimeHelpers::get_OffsetToStringData", RuntimeHelpers_get_OffsetToStringData },
    { "System.Runtime.CompilerServices.RuntimeHelpers::GetObjectValue", RuntimeHelpers_GetObjectValue },
    { "System.Runtime.CompilerServices.RuntimeHelpers::RunClassConstructor", RuntimeHelpers_RunClassConstructor },
    { "System.Math::Floor", Math_Floor },
    { "System.Math::Ceiling", Math_Ceiling },
    { "System.Math::Round(double)", Math_Round },
    { "System.Math::Round2", Math_Round2 },
    { "System.Math::Sqrt", Math_Sqrt },
    { "System.Math::Pow", Math_Pow },
    { "System.Math::Sin", Math_Sin },
    { "System.Math::Cos", Math_Cos },
    { "System.Math::Tan", Math_Tan },
    { "System.Math::Asin", Math_Asin },
    { "System.Math::Acos", Math_Acos },
    { "System.Math::Atan", Math_Atan },
    { "System.Math::Atan2", Math_Atan2 },
    { "System.Math::Sinh", Math_Sinh },
    { "System.Math::Cosh", Math_Cosh },
    { "System.Math::Tanh", Math_Tanh },
    { "System.Math::Exp", Math_Exp },
    { "System.Math::Log", Math_Log },
    { "System.Math::Log10", Math_Log10 },
    { "System.Math::Abs(double)", Math_Abs_d },
    { "System.Math::Abs(single)", Math_Abs_f },
    { "System.Environment::get_NewLine", Environment_get_NewLine },
    { "System.Environment::get_Platform", Environment_get_Platform },
    { "System.Environment::get_TickCount", Environment_get_TickCount },
    { "System.Environment::get_ProcessorCount", Environment_get_ProcessorCount },
    { "System.Environment::internalGetEnvironmentVariable", Environment_internalGetEnvironmentVariable },
    { "System.Environment::GetMachineConfigPath", Environment_GetMachineConfigPath },
    { "System.Environment::get_MachineName", Environment_get_MachineName },
    { "System.Environment::get_UserName", Environment_get_UserName },
    { "System.Environment::GetOSVersionString", Environment_GetOSVersionString },
    { "System.Environment::get_HasShutdownStarted", Environment_get_HasShutdownStarted },
    { "System.Environment::Exit", Environment_Exit },
    { "System.Environment::get_ExitCode", Environment_get_ExitCode },
    { "System.Environment::set_ExitCode", Environment_set_ExitCode },
    { "System.Environment::GetCommandLineArgs", Environment_GetCommandLineArgs },
    { "System.Environment::GetEnvironmentVariableNames", Environment_GetEnvironmentVariableNames },
    { "System.Environment::InternalSetEnvironmentVariable", Environment_InternalSetEnvironmentVariable },
    { "System.Environment::GetWindowsFolderPath", Environment_GetWindowsFolderPath },
    { "System.Environment::GetLogicalDrivesInternal", Environment_GetLogicalDrivesInternal },
    { "System.Environment::internalBroadcastSettingChange", Environment_internalBroadcastSettingChange },
    { "System.DateTime::GetNow", DateTime_GetNow },
    { "System.DateTime::GetTimeMonotonic", DateTime_GetTimeMonotonic },
    { "System.Diagnostics.Stopwatch::GetTimestamp", Stopwatch_GetTimestamp },
    { "System.CurrentSystemTimeZone::GetTimeZoneData", TimeZone_GetTimeZoneData },
    { "System.GC::SuppressFinalize", GC_SuppressFinalize },
    { "System.GC::ReRegisterForFinalize", GC_ReRegisterForFinalize },
    { "System.GC::InternalCollect", GC_InternalCollect },
    { "System.GC::GetTotalMemory", GC_GetTotalMemory },
    { "System.GC::get_MaxGeneration", GC_get_MaxGeneration },
    { "System.GC::KeepAlive", GC_KeepAlive },
    { "System.GC::WaitForPendingFinalizers", GC_WaitForPendingFinalizers },
    { "System.GC::CollectionCount", GC_CollectionCount },
    { "System.Runtime.InteropServices.GCHandle::GetTargetHandle", GCHandle_GetTargetHandle },
    { "System.Runtime.InteropServices.GCHandle::GetTarget", GCHandle_GetTarget },
    { "System.Runtime.InteropServices.GCHandle::FreeHandle", GCHandle_FreeHandle },
    { "System.Runtime.InteropServices.GCHandle::GetAddrOfPinnedObject", GCHandle_GetAddrOfPinnedObject },
    { "System.Runtime.InteropServices.GCHandle::CheckCurrentDomain", GCHandle_CheckCurrentDomain },
    { "System.Runtime.InteropServices.Marshal::AllocHGlobal", Marshal_AllocHGlobal },
    { "System.Runtime.InteropServices.Marshal::FreeHGlobal", Marshal_FreeHGlobal },
    { "System.Runtime.InteropServices.Marshal::AllocCoTaskMem", Marshal_AllocCoTaskMem },
    { "System.Runtime.InteropServices.Marshal::FreeCoTaskMem", Marshal_FreeCoTaskMem },
    { "System.Runtime.InteropServices.Marshal::ReadByte", Marshal_ReadByte },
    { "System.Runtime.InteropServices.Marshal::ReadInt16", Marshal_ReadInt16 },
    { "System.Runtime.InteropServices.Marshal::ReadInt32", Marshal_ReadInt32 },
    { "System.Runtime.InteropServices.Marshal::ReadInt64", Marshal_ReadInt64 },
    { "System.Runtime.InteropServices.Marshal::ReadIntPtr", Marshal_ReadIntPtr },
    { "System.Runtime.InteropServices.Marshal::WriteByte", Marshal_WriteByte },
    { "System.Runtime.InteropServices.Marshal::WriteInt16(intptr,int,int16)", Marshal_WriteInt16 },
    { "System.Runtime.InteropServices.Marshal::WriteInt32", Marshal_WriteInt32 },
    { "System.Runtime.InteropServices.Marshal::WriteInt64", Marshal_WriteInt64 },
    { "System.Runtime.InteropServices.Marshal::WriteIntPtr", Marshal_WriteIntPtr },
    { "System.Runtime.InteropServices.Marshal::copy_to_unmanaged", Marshal_copy_to_unmanaged },
    { "System.Runtime.InteropServices.Marshal::copy_from_unmanaged", Marshal_copy_from_unmanaged },
    { "System.Runtime.InteropServices.Marshal::PtrToStringAnsi(intptr)", Marshal_PtrToStringAnsi },
    { "System.Runtime.InteropServices.Marshal::PtrToStringAnsi(intptr,int)", Marshal_PtrToStringAnsi2 },
    { "System.Runtime.InteropServices.Marshal::PtrToStringUni(intptr)", Marshal_PtrToStringUni },
    { "System.Runtime.InteropServices.Marshal::PtrToStringUni(intptr,int)", Marshal_PtrToStringUni2 },
    { "System.Runtime.InteropServices.Marshal::StringToHGlobalAnsi", Marshal_StringToHGlobalAnsi },
    { "System.Runtime.InteropServices.Marshal::StringToHGlobalUni", Marshal_StringToHGlobalUni },
    { "System.Runtime.InteropServices.Marshal::GetLastWin32Error", Marshal_GetLastWin32Error },
    { "System.Runtime.InteropServices.Marshal::SizeOf", Marshal_SizeOf },
    { "System.Runtime.InteropServices.Marshal::UnsafeAddrOfPinnedArrayElement", Marshal_UnsafeAddrOfPinnedArrayElement },
    { "System.Runtime.InteropServices.Marshal::StructureToPtr", Marshal_StructureToPtr },
    { "System.Runtime.InteropServices.Marshal::PtrToStructure(intptr,System.Type)", Marshal_PtrToStructure },
    { "System.Runtime.InteropServices.Marshal::PtrToStructure(intptr,object)", Marshal_PtrToStructureObject },
    { "System.Threading.Thread::CurrentThread_internal", Thread_CurrentThread_internal },
    { "System.Threading.Thread::Thread_init", Thread_Thread_init },
    { "System.Threading.Thread::GetCachedCurrentCulture", Thread_GetCachedCurrentCulture },
    { "System.Threading.Thread::SetCachedCurrentCulture", Thread_SetCachedCurrentCulture },
    { "System.Threading.Thread::GetCachedCurrentUICulture", Thread_GetNull },
    { "System.Threading.Thread::SetCachedCurrentUICulture", Thread_SetIgnore },
    { "System.Threading.Thread::GetSerializedCurrentCulture", Thread_GetNull },
    { "System.Threading.Thread::SetSerializedCurrentCulture", Thread_SetIgnore },
    { "System.Threading.Thread::GetSerializedCurrentUICulture", Thread_GetNull },
    { "System.Threading.Thread::SetSerializedCurrentUICulture", Thread_SetIgnore },
    { "System.Threading.Thread::GetDomainID", Thread_GetDomainID },
    { "System.Reflection.FieldInfo::GetUnmanagedMarshal", FieldInfo_GetUnmanagedMarshal },
    { "System.Diagnostics.FileVersionInfo::GetVersionInfo_internal", FileVersionInfo_GetVersionInfo_internal },
    { "System.Configuration.InternalConfigurationHost::get_bundled_machine_config", InternalConfigurationHost_get_bundled_machine_config },
    { "System.Threading.Thread::GetNewManagedId_internal", Thread_GetNewManagedId_internal },
    { "System.Threading.Thread::Thread_free_internal", Thread_Thread_free_internal },
    { "System.Threading.Thread::Sleep_internal", Thread_Sleep_internal },
    { "System.Threading.Thread::MemoryBarrier", Thread_MemoryBarrier },
    { "System.Threading.Thread::SpinWait_nop", Thread_SpinWait_nop },
    { "System.Threading.Thread::GetName_internal", Thread_GetName_internal },
    { "System.Threading.Thread::SetName_internal", Thread_SetName_internal },
    { "System.Threading.Thread::GetState", Thread_GetState },
    { "System.Threading.Thread::SetState", Thread_SetState },
    { "System.Threading.Thread::ClrState", Thread_ClrState },
    { "System.Threading.Thread::ResetAbort_internal", Thread_ResetAbort_internal },
    { "System.Threading.Thread::FreeLocalSlotValues", Thread_FreeLocalSlotValues },
    { "System.Threading.Thread::Thread_internal", Thread_Thread_internal },
    { "System.Threading.Thread::Join_internal", Thread_Join_internal },
    { "System.Threading.Thread::Abort_internal", Thread_Abort_internal },
    { "System.Threading.Thread::Interrupt_internal", Thread_Interrupt_internal },
    { "System.Threading.Thread::VolatileRead(int&)", VR_i4 },
    { "System.Threading.Thread::VolatileRead(uint&)", VR_i4 },
    { "System.Threading.Thread::VolatileRead(long&)", VR_i8 },
    { "System.Threading.Thread::VolatileRead(ulong&)", VR_i8 },
    { "System.Threading.Thread::VolatileRead(double&)", VR_r8 },
    { "System.Threading.Thread::VolatileRead(single&)", VR_r4 },
    { "System.Threading.Thread::VolatileRead(byte&)", VR_u1 },
    { "System.Threading.Thread::VolatileRead(sbyte&)", VR_u1 },
    { "System.Threading.Thread::VolatileRead(int16&)", VR_i2 },
    { "System.Threading.Thread::VolatileRead(uint16&)", VR_i2 },
    { "System.Threading.Thread::VolatileRead(intptr&)", VR_ip },
    { "System.Threading.Thread::VolatileRead(uintptr&)", VR_ip },
    { "System.Threading.Thread::VolatileRead(object&)", VR_ref },
    { "System.Threading.Thread::VolatileWrite(int&,int)", VW_i4 },
    { "System.Threading.Thread::VolatileWrite(uint&,uint)", VW_i4 },
    { "System.Threading.Thread::VolatileWrite(long&,long)", VW_i8 },
    { "System.Threading.Thread::VolatileWrite(ulong&,ulong)", VW_i8 },
    { "System.Threading.Thread::VolatileWrite(double&,double)", VW_r8 },
    { "System.Threading.Thread::VolatileWrite(single&,single)", VW_r4 },
    { "System.Threading.Thread::VolatileWrite(byte&,byte)", VW_u1 },
    { "System.Threading.Thread::VolatileWrite(sbyte&,sbyte)", VW_u1 },
    { "System.Threading.Thread::VolatileWrite(int16&,int16)", VW_i2 },
    { "System.Threading.Thread::VolatileWrite(uint16&,uint16)", VW_i2 },
    { "System.Threading.Thread::VolatileWrite(intptr&,intptr)", VW_ip },
    { "System.Threading.Thread::VolatileWrite(uintptr&,uintptr)", VW_ip },
    { "System.Threading.Thread::VolatileWrite(object&,object)", VW_ref },
    { "System.Threading.Interlocked::CompareExchange(int&,int,int)", IL_CmpXchg_i4 },
    { "System.Threading.Interlocked::CompareExchange(object&,object,object)", IL_CmpXchg_ref },
    { "System.Threading.Interlocked::CompareExchange(single&,single,single)", IL_CmpXchg_r4 },
    { "System.Threading.Interlocked::CompareExchange(long&,long,long)", IL_CmpXchg_i8 },
    { "System.Threading.Interlocked::CompareExchange(intptr&,intptr,intptr)", IL_CmpXchg_ip },
    { "System.Threading.Interlocked::CompareExchange(double&,double,double)", IL_CmpXchg_r8 },
    { "System.Threading.Interlocked::CompareExchange(T&,T,T)", IL_CmpXchg_T },
    { "System.Threading.Interlocked::Exchange(int&,int)", IL_Xchg_i4 },
    { "System.Threading.Interlocked::Exchange(object&,object)", IL_Xchg_ref },
    { "System.Threading.Interlocked::Exchange(single&,single)", IL_Xchg_r4 },
    { "System.Threading.Interlocked::Exchange(long&,long)", IL_Xchg_i8 },
    { "System.Threading.Interlocked::Exchange(intptr&,intptr)", IL_Xchg_ip },
    { "System.Threading.Interlocked::Exchange(double&,double)", IL_Xchg_r8 },
    { "System.Threading.Interlocked::Exchange(T&,T)", IL_Xchg_T },
    { "System.Threading.Interlocked::Increment(int&)", IL_Inc_i4 },
    { "System.Threading.Interlocked::Increment(long&)", IL_Inc_i8 },
    { "System.Threading.Interlocked::Decrement(int&)", IL_Dec_i4 },
    { "System.Threading.Interlocked::Decrement(long&)", IL_Dec_i8 },
    { "System.Threading.Interlocked::Add(int&,int)", IL_Add_i4 },
    { "System.Threading.Interlocked::Add(long&,long)", IL_Add_i8 },
    { "System.Threading.Interlocked::Read(long&)", IL_Read_i8 },
    { "System.Threading.Monitor::Enter", Monitor_Enter },
    { "System.Threading.Monitor::Exit", Monitor_Exit },
    { "System.Threading.Monitor::Monitor_try_enter", Monitor_try_enter },
    { "System.Threading.Monitor::Monitor_test_owner", Monitor_test_owner },
    { "System.Threading.Monitor::Monitor_test_synchronised", Monitor_test_synchronised },
    { "System.Threading.Monitor::Monitor_pulse", Monitor_pulse },
    { "System.Threading.Monitor::Monitor_pulse_all", Monitor_pulse_all },
    { "System.Threading.Monitor::Monitor_wait", Monitor_wait },
    { "System.Threading.NativeEventCalls::CreateEvent_internal", Event_Create },
    { "System.Threading.NativeEventCalls::SetEvent_internal", Event_Set },
    { "System.Threading.NativeEventCalls::ResetEvent_internal", Event_Reset },
    { "System.Threading.NativeEventCalls::CloseEvent_internal", Event_Close },
    { "System.Threading.WaitHandle::WaitOne_internal", WaitHandle_WaitOne_internal },
    { "System.Threading.WaitHandle::WaitAny_internal", WaitHandle_WaitAny_internal },
    { "System.Threading.WaitHandle::WaitAll_internal", WaitHandle_WaitAll_internal },
    { "System.Threading.WaitHandle::SignalAndWait_Internal", WaitHandle_SignalAndWait_Internal },
    { "System.Threading.Mutex::CreateMutex_internal", Mutex_Create },
    { "System.Threading.Mutex::ReleaseMutex_internal", Mutex_Release },
    { "System.Threading.Semaphore::CreateSemaphore_internal", Semaphore_Create },
    { "System.Threading.Semaphore::ReleaseSemaphore_internal", Semaphore_Release },
    { "System.IO.MonoIO::Open", MonoIO_Open },
    { "System.IO.MonoIO::Close", MonoIO_Close },
    { "System.IO.MonoIO::Read", MonoIO_Read },
    { "System.IO.MonoIO::Write", MonoIO_Write },
    { "System.IO.MonoIO::Seek", MonoIO_Seek },
    { "System.IO.MonoIO::Flush", MonoIO_Flush },
    { "System.IO.MonoIO::GetLength", MonoIO_GetLength },
    { "System.IO.MonoIO::SetLength", MonoIO_SetLength },
    { "System.IO.MonoIO::GetFileType", MonoIO_GetFileType },
    { "System.IO.MonoIO::GetFileAttributes", MonoIO_GetFileAttributes },
    { "System.IO.MonoIO::SetFileAttributes", MonoIO_SetFileAttributes },
    { "System.IO.MonoIO::GetFileStat", MonoIO_GetFileStat },
    { "System.IO.MonoIO::CreateDirectory", MonoIO_CreateDirectory },
    { "System.IO.MonoIO::RemoveDirectory", MonoIO_RemoveDirectory },
    { "System.IO.MonoIO::DeleteFile", MonoIO_DeleteFile },
    { "System.IO.MonoIO::MoveFile", MonoIO_MoveFile },
    { "System.IO.MonoIO::CopyFile", MonoIO_CopyFile },
    { "System.IO.MonoIO::ReplaceFile", MonoIO_ReplaceFile },
    { "System.IO.MonoIO::GetCurrentDirectory", MonoIO_GetCurrentDirectory },
    { "System.IO.MonoIO::SetCurrentDirectory", MonoIO_SetCurrentDirectory },
    { "System.IO.MonoIO::GetFileSystemEntries", MonoIO_GetFileSystemEntries },
    { "System.IO.MonoIO::get_ConsoleOutput", MonoIO_get_ConsoleOutput },
    { "System.IO.MonoIO::get_ConsoleInput", MonoIO_get_ConsoleInput },
    { "System.IO.MonoIO::get_ConsoleError", MonoIO_get_ConsoleError },
    { "System.IO.MonoIO::get_VolumeSeparatorChar", MonoIO_get_VolumeSeparatorChar },
    { "System.IO.MonoIO::get_DirectorySeparatorChar", MonoIO_get_DirectorySeparatorChar },
    { "System.IO.MonoIO::get_AltDirectorySeparatorChar", MonoIO_get_AltDirectorySeparatorChar },
    { "System.IO.MonoIO::get_PathSeparator", MonoIO_get_PathSeparator },
    { "System.IO.MonoIO::get_InvalidPathChars", MonoIO_get_InvalidPathChars },
    { "System.IO.MonoIO::Lock", MonoIO_Lock },
    { "System.IO.MonoIO::Unlock", MonoIO_Unlock },
    { "System.IO.MonoIO::SetFileTime", MonoIO_SetFileTime },
    { "System.IO.Path::get_temp_path", Path_get_temp_path },
    { "System.Text.Encoding::InternalCodePage", Encoding_InternalCodePage },
    { "System.Globalization.CultureInfo::construct_internal_locale_from_current_locale", CultureInfo_construct_internal_locale_from_current_locale },
    { "System.Globalization.CultureInfo::construct_internal_locale_from_lcid", CultureInfo_false2 },
    { "System.Globalization.CultureInfo::construct_internal_locale_from_name", CultureInfo_from_name },
    { "System.Globalization.CultureInfo::construct_internal_locale_from_specific_name", CultureInfo_from_name },
    { "System.Globalization.CultureInfo::construct_datetime_format", CultureInfo_void1 },
    { "System.Globalization.CultureInfo::construct_number_format", CultureInfo_void1 },
    { "System.Globalization.CultureInfo::internal_is_lcid_neutral", CultureInfo_false2 },
    { "System.Security.Cryptography.RNGCryptoServiceProvider::RngOpen", Rng_Open },
    { "System.Security.Cryptography.RNGCryptoServiceProvider::RngInitialize", Rng_Initialize },
    { "System.Security.Cryptography.RNGCryptoServiceProvider::RngGetBytes", Rng_GetBytes },
    { "System.Security.Cryptography.RNGCryptoServiceProvider::RngClose", Rng_Close },
    { "System.Globalization.CompareInfo::internal_compare", CompareInfo_internal_compare },
    { "System.Globalization.CompareInfo::internal_index(string,int,int,char,System.Globalization.CompareOptions,bool)", CompareInfo_internal_index_char },
    { "System.Globalization.CompareInfo::internal_index(string,int,int,string,System.Globalization.CompareOptions,bool)", CompareInfo_internal_index_str },
    { "System.Globalization.CompareInfo::construct_compareinfo", CompareInfo_construct_compareinfo },
    { "System.Globalization.CompareInfo::free_internal_collator", CompareInfo_free_internal_collator },
    { "System.Globalization.CompareInfo::assign_sortkey", CompareInfo_assign_sortkey },
    { "System.Char::GetDataTablePointers", Char_GetDataTablePointers },
    { "System.Double::ParseImpl", Double_ParseImpl },
    { "System.Security.SecurityManager::get_SecurityEnabled", SecurityManager_get_SecurityEnabled },
    { "System.AppDomain::getCurDomain", AppDomain_getCurDomain },
    { "System.AppDomain::getRootDomain", AppDomain_getCurDomain },
    { "System.AppDomain::getFriendlyName", AppDomain_getFriendlyName },
    { "System.AppDomain::GetData", AppDomain_GetData },
    { "System.AppDomain::SetData", AppDomain_SetData },
    { "System.AppDomain::getSetup", AppDomain_getSetup },
    { "System.AppDomain::InternalGetContext", AppDomain_InternalGetContext },
    { "System.AppDomain::InternalGetDefaultContext", AppDomain_InternalGetContext },
    { "System.AppDomain::InternalSetContext", AppDomain_InternalSetContext },
    { "System.AppDomain::getDomainID", AppDomain_getDomainID },
};

void nr_register_corlib_icalls(void)
{
    InitializeCriticalSection(&intern_lock);
    (void)NumberFormatter_dummy;
    (void)Path_GetTempPath_len;
    nr_register_icalls(corlib, (int)(sizeof corlib / sizeof corlib[0]));
    nr_register_reflect_icalls();
    nr_register_net_icalls();
    nr_register_decimal_icalls();
    nr_register_corlib_icalls2();
#if NR_CORLIB4
    nr_register_corlib4_icalls();
#endif
}

/* NumberFormatter.GetFormatterTables: the tables come from the generated fmttab.c */
extern const uint64_t nr_fmt_mantissa[];
extern const int32_t nr_fmt_tens[];
extern const uint16_t nr_fmt_digit_lower[], nr_fmt_digit_upper[];
extern const int64_t nr_fmt_ten_powers[];
extern const int32_t nr_fmt_dec_hex[];
static void NumberFormatter_GetFormatterTables(const uint64_t** m, const int32_t** t, const uint16_t** dl, const uint16_t** du,
                                               const int64_t** tp, const int32_t** dh)
{
    *m = nr_fmt_mantissa; *t = nr_fmt_tens; *dl = nr_fmt_digit_lower; *du = nr_fmt_digit_upper; *tp = nr_fmt_ten_powers; *dh = nr_fmt_dec_hex;
}
void nr_register_corlib_icalls2(void)
{
    static const NrIcall t[] = {
        { "System.NumberFormatter::GetFormatterTables", NumberFormatter_GetFormatterTables },
    };
    nr_register_icalls(t, (int)(sizeof t / sizeof t[0]));
}

/* ==== mono 4.x class library (Unity 2017+, "MonoBleedingEdge") ====
 * The 4.x profile renamed or reshaped many internal calls: these are the
 * ones whose names or signatures differ from 2.x. Registered after the 2.x
 * table, so a key both define takes this version. Reflection's share is in
 * nr_reflect.c. */
#if NR_CORLIB4
#include <oleauto.h>
#pragma comment(lib, "oleaut32.lib")

static void* String_FastAllocateString(int32_t len) { return nr_str_alloc(len); }

static void Monitor_try_enter_with_atomic_var(void* o, int32_t ms, uint8_t* taken)
{
    if (Monitor_try_enter(o, ms)) *taken = 1;
}

/* Interlocked.CompareExchange(ref int, int, int, ref bool succeeded) */
static int32_t IL_CmpXchg_i4b(int32_t* p, int32_t v, int32_t c, uint8_t* ok)
{
    int32_t r = InterlockedCompareExchange((volatile LONG*)p, v, c);
    *ok = r == c;
    return r;
}

/* Volatile.Read/Write: bool, int and reference-typed T (the generic ones get T's class last) */
static int32_t Vol_Read_u1(uint8_t* p) { MemoryBarrier(); return *(volatile uint8_t*)p; }
static void* Vol_Read_T(void** p, NrClass* t) { (void)t; MemoryBarrier(); return *(void* volatile*)p; }
static void Vol_Write_T(void** p, void* v, NrClass* t) { (void)t; *(void* volatile*)p = v; MemoryBarrier(); }

/* ---- threads ---- */
static void Thread_ConstructInternalThread(void* self) { internal_of(self); }
static void* Thread_GetCurrentThread(void) { return nr_current_thread_object(); }
static void Thread_SleepInternal(int32_t ms) { Sleep(ms < 0 ? INFINITE : (DWORD)ms); }
static uint8_t Thread_YieldInternal(void) { return SwitchToThread() != 0; }
static int32_t Thread_SystemMaxStackStize(void) { return 1 << 24; }
static uint8_t Thread_JoinInternal(void* self, int32_t ms)
{
    intptr_t h = FIELD(internal_of(self), intptr_t, off_handle);
    if (!h) return 1;
    return WaitForSingleObject((HANDLE)h, ms < 0 ? INFINITE : (DWORD)ms) == WAIT_OBJECT_0;
}
static void Thread_SetPriorityNative(void* self, int32_t p) { (void)self; (void)p; }
static int32_t Thread_GetPriorityNative(void* self) { (void)self; return 2; }
static void* Thread_GetAbortExceptionState(void* self) { (void)self; return 0; }
static void* Thread_ByteArrayToDomain(void* a) { return a; }
static void Thread_InterruptInternal(void* self) { (void)self; }

/* ---- thread pool: mono 4.x runs the managed work queue on native workers ----
 * RequestWorkerThread asks for a worker; each one calls
 * _ThreadPoolWaitCallback.PerformWaitCallback until it says the queue is dry.
 * ponytail: up to one worker per core, no hill-climbing; mono adds threads
 * when work starves, add that if a title queues blocking work items. */
static volatile LONG tp_idle, tp_threads, tp_pending;
static HANDLE tp_sem;
static const NrMethodDesc* tp_perform;
static DWORD WINAPI tp_worker(LPVOID p)
{
    (void)p;
    nr_thread_attach();
    for (;;)
    {
        InterlockedIncrement(&tp_idle);
        DWORD r = WaitForSingleObject(tp_sem, 20000);
        InterlockedDecrement(&tp_idle);
        if (r != WAIT_OBJECT_0) continue;
        InterlockedDecrement(&tp_pending);
        void* ex = 0;
        nr_invoke(tp_perform, 0, 0, &ex);
        if (ex && nr_unhandled_hook) nr_unhandled_hook(ex);
    }
    return 0;
}
static uint8_t ThreadPool_RequestWorkerThread(void)
{
    if (!tp_perform)
    {
        NrClass* k = nr_class_by_name("System.Threading", "_ThreadPoolWaitCallback");
        tp_perform = k ? nr_method(k, "PerformWaitCallback", 0) : 0;
        if (!tp_perform || !tp_perform->fn) { nr_log("netrecomp: _ThreadPoolWaitCallback.PerformWaitCallback not compiled\n"); return 0; }
        tp_sem = CreateSemaphoreA(0, 0, 0x7fffffff, 0);
    }
    /* a worker is already on its way for every request not yet taken */
    if (tp_pending > tp_idle + 1) return 1;
    InterlockedIncrement(&tp_pending);
    SYSTEM_INFO si; GetSystemInfo(&si);
    if (tp_idle == 0 && tp_threads < (LONG)(si.dwNumberOfProcessors < 2 ? 2 : si.dwNumberOfProcessors))
    {
        InterlockedIncrement(&tp_threads);
        CloseHandle(CreateThread(0, 0, tp_worker, 0, 0, 0));
    }
    ReleaseSemaphore(tp_sem, 1, 0);
    return 1;
}
static void ThreadPool_InitializeVMTp(uint8_t* tracking) { *tracking = 0; }
static uint8_t ThreadPool_false(void) { return 0; }
static uint8_t ThreadPool_true(void) { return 1; }
static void ThreadPool_void(void) {}
static void ThreadPool_ReportThreadStatus(uint8_t w) { (void)w; }
static uint8_t ThreadPool_SetThreads(int32_t a, int32_t b) { (void)a; (void)b; return 1; }
static void ThreadPool_GetThreads(int32_t* a, int32_t* b) { *a = 32; *b = 32; }
static uint8_t ThreadPool_PostQueuedCompletionStatus(void* o) { (void)o; return 0; }
static uint8_t ThreadPool_BindIO(intptr_t h) { (void)h; return 1; }

static int64_t Timer_GetTimeMonotonic(void) { return DateTime_GetTimeMonotonic(); }

/* ---- wait handles ---- */
static intptr_t Event_Create4(uint8_t manual, uint8_t initial, void* name, int32_t* err)
{
    HANDLE h = CreateEventW(0, manual, initial, name ? (LPCWSTR)STR(name)->chars : 0);
    *err = (int32_t)GetLastError();
    if (*err != ERROR_ALREADY_EXISTS) *err = 0;
    return (intptr_t)h;
}
static int32_t WaitHandle_Wait_internal(intptr_t* hs, int32_t n, uint8_t all, int32_t ms)
{
    DWORD r = n == 1 ? WaitForSingleObjectEx((HANDLE)hs[0], ms < 0 ? INFINITE : (DWORD)ms, FALSE)
                     : WaitForMultipleObjectsEx((DWORD)n, (const HANDLE*)hs, all, ms < 0 ? INFINITE : (DWORD)ms, FALSE);
    if (r == WAIT_TIMEOUT) return 258;
    if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + (DWORD)n) return 0x80 + (int32_t)(r - WAIT_ABANDONED_0);
    return (int32_t)(r - WAIT_OBJECT_0);
}
static int32_t WaitHandle_SignalAndWait4(intptr_t s, intptr_t w, int32_t ms)
{
    DWORD r = SignalObjectAndWait((HANDLE)s, (HANDLE)w, ms < 0 ? INFINITE : (DWORD)ms, FALSE);
    return r == WAIT_TIMEOUT ? 258 : 0;
}
static intptr_t Mutex_Create4(uint8_t owned, void* name, uint8_t* created)
{
    HANDLE h = CreateMutexW(0, owned, name ? (LPCWSTR)STR(name)->chars : 0);
    *created = GetLastError() != ERROR_ALREADY_EXISTS;
    return (intptr_t)h;
}
static intptr_t Semaphore_Create4(int32_t init, int32_t max, void* name, int32_t* err)
{
    HANDLE h = CreateSemaphoreW(0, init, max, name ? (LPCWSTR)STR(name)->chars : 0);
    *err = GetLastError() == ERROR_ALREADY_EXISTS ? ERROR_ALREADY_EXISTS : 0;
    return (intptr_t)h;
}
static uint8_t Semaphore_Release4(intptr_t h, int32_t n, int32_t* prev)
{
    LONG p = 0;
    BOOL ok = ReleaseSemaphore((HANDLE)h, n, &p);
    *prev = (int32_t)p;
    return ok != 0;
}

/* ---- GC ---- */
static void* GC_get_ephemeron_tombstone(void)
{
    static void* tomb;
    if (!tomb) { tomb = nr_new(nr_k_object); nr_gchandle_new(tomb, 0); }
    return tomb;
}
/* ConditionalWeakTable's ephemerons. ponytail: kept as ordinary strong
 * references (keys never die while their table lives); weak ephemerons if a
 * title leaks through ConditionalWeakTable. */
static void GC_register_ephemeron_array(void* a) { (void)a; }
static int32_t GC_GetCollectionCount(int32_t g) { (void)g; return (int32_t)GC_get_gc_no(); }
static int32_t GC_GetGeneration(void* o) { (void)o; return 0; }
static void GC_RecordPressure(int64_t n) { (void)n; }

/* ---- time ---- */
static int64_t DateTime_GetSystemTimeAsFileTime(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}
/* CurrentSystemTimeZone.GetTimeZoneData(int year, out long[] data, out string[] names, out bool daylight_inverted) */
static uint8_t TimeZone_GetTimeZoneData4(int32_t year, void** data, void** names, uint8_t* inverted)
{
    *inverted = 0;
    return TimeZone_GetTimeZoneData(year, data, names);
}

/* ---- environment ---- */
static void* Environment_GetNewLine(void) { return nr_str_utf8("\r\n"); }
static int32_t Environment_GetPageSize(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int32_t)si.dwPageSize; }
static void* Environment_internalGetEnvironmentVariable_native(intptr_t utf8)
{
    if (!utf8) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)utf8, -1, 0, 0);
    wchar_t* w = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, (const char*)utf8, -1, w, n);
    void* r = Environment_internalGetEnvironmentVariable(nr_str_utf16((uint16_t*)w, n - 1));
    free(w);
    return r;
}
static void* Environment_internalGetHome(void) { return Environment_GetWindowsFolderPath(0x28); /* CSIDL_PROFILE */ }
static uint8_t CLRConfig_false(void) { return 0; }
static void* Process_ProcessName_internal(intptr_t h)
{
    (void)h;
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(0, buf, MAX_PATH);
    wchar_t* b = buf + n;
    while (b > buf && b[-1] != '\\') b--;
    wchar_t* dot = wcsrchr(b, '.');
    if (dot) *dot = 0;
    return nr_str_utf16((uint16_t*)b, (int32_t)wcslen(b));
}
static intptr_t NativeMethods_GetCurrentProcess(void) { return (intptr_t)GetCurrentProcess(); }
static int32_t NativeMethods_GetCurrentProcessId(void) { return (int32_t)GetCurrentProcessId(); }
static uint8_t NativeMethods_GetExitCodeProcess(intptr_t h, int32_t* code) { DWORD c = 0; BOOL ok = GetExitCodeProcess((HANDLE)h, &c); *code = (int32_t)c; return ok != 0; }
static uint8_t NativeMethods_CloseProcess(intptr_t h) { return CloseHandle((HANDLE)h) != 0; }
static uint8_t Debugger_false(void) { return 0; }
static void Debugger_Log(int32_t level, void* cat, void* msg) { (void)level; (void)cat; if (msg) OutputDebugStringW((LPCWSTR)STR(msg)->chars); }
static void DefaultTraceListener_WriteWindowsDebugString(void* msg) { if (msg) OutputDebugStringW((LPCWSTR)STR(msg)->chars); }

/* SafeStringMarshal: UTF-8 copies the class library hands to icalls */
static intptr_t SafeStringMarshal_StringToUtf8(void* s) { return (intptr_t)(s ? nr_utf8(s) : 0); }
static void SafeStringMarshal_GFree(intptr_t p) { nr_free((void*)p); }
static void GPtrArray_Free(void* a)
{
    if (!a) return;
    free(((void**)a)[0]);
    free(a);
}


/* ---- Buffer / Math ---- */
static uint8_t Buffer_InternalBlockCopy(void* src, int32_t so, void* dst, int32_t d0, int32_t count) { return Buffer_BlockCopyInternal(src, so, dst, d0, count); }
static int32_t Buffer_ByteLength4(void* a) { return Buffer_ByteLengthInternal(a); }
static double Math_SplitFractionDouble(double* v) { double ip; double f = modf(*v, &ip); *v = ip; return f; }
static double Math_Round4(double d) { return Math_Round(d); }

/* ---- Decimal: the .NET layout {flags, hi, lo, mid} is OLE's DECIMAL ---- */
static void dec_check(HRESULT hr) { if (hr == DISP_E_OVERFLOW) nr_throw_new("System", "OverflowException", "Value was either too large or too small for a Decimal."); if (hr == DISP_E_DIVBYZERO) nr_throw_divzero(); }
static void Decimal_ctor_r8(DECIMAL* d, double v) { dec_check(VarDecFromR8(v, d)); }
static void Decimal_ctor_r4(DECIMAL* d, float v) { dec_check(VarDecFromR4(v, d)); }
static void Decimal_FCallAddSub(DECIMAL* a, DECIMAL* b, uint8_t sub)
{
    DECIMAL r;
    if (sub) { DECIMAL nb; VarDecNeg(b, &nb); dec_check(VarDecAdd(a, &nb, &r)); }
    else dec_check(VarDecAdd(a, b, &r));
    *a = r;
}
static void Decimal_FCallMultiply(DECIMAL* a, DECIMAL* b) { DECIMAL r; dec_check(VarDecMul(a, b, &r)); *a = r; }
static void Decimal_FCallDivide(DECIMAL* a, DECIMAL* b) { DECIMAL r; dec_check(VarDecDiv(a, b, &r)); *a = r; }
static int32_t Decimal_FCallCompare(DECIMAL* a, DECIMAL* b) { return (int32_t)VarDecCmp(a, b) - 1; }
static void Decimal_FCallFloor(DECIMAL* a) { DECIMAL r; VarDecInt(a, &r); *a = r; }
static void Decimal_FCallTruncate(DECIMAL* a) { DECIMAL r; VarDecFix(a, &r); *a = r; }
static void Decimal_FCallRound(DECIMAL* a, int32_t n) { DECIMAL r; dec_check(VarDecRound(a, n, &r)); *a = r; }
static int32_t Decimal_FCallToInt32(DECIMAL d) { LONG v; DECIMAL t; VarDecFix(&d, &t); if (VarI4FromDec(&t, &v)) nr_throw_new("System", "OverflowException", "Value was either too large or too small for an Int32."); return v; }
static double Decimal_ToDouble(DECIMAL d) { double v; VarR8FromDec(&d, &v); return v; }
static float Decimal_ToSingle(DECIMAL d) { float v; VarR4FromDec(&d, &v); return v; }
static int32_t Decimal_GetHashCode(DECIMAL* d) { double v; VarR8FromDec(d, &v); if (v == 0) return 0; int64_t b; memcpy(&b, &v, 8); return (int32_t)b ^ (int32_t)(b >> 32); }

/* Number.NumberBufferToDouble(byte* numberBuffer, ref double value): the
 * buffer PackForNative hands over is { int precision; int scale; int sign; }
 * followed by the NUL-terminated UTF-16 digits. */
typedef struct NumBuf { int32_t precision, scale, sign; uint16_t digits[1]; } NumBuf;
static uint8_t Number_NumberBufferToDouble(NumBuf* nb, double* v)
{
    char buf[400]; int n = 0;
    if (nb->sign) buf[n++] = '-';
    buf[n++] = '0'; buf[n++] = '.';
    const uint16_t* d = nb->digits;
    while (*d && n < 360) buf[n++] = (char)*d++;
    n += sprintf(buf + n, "e%d", nb->scale);
    buf[n] = 0;
    *v = strtod(buf, 0);
    return 1;
}
static uint8_t Number_NumberBufferToDecimal(NumBuf* nb, DECIMAL* v)
{
    wchar_t buf[400]; int n = 0;
    if (nb->sign) buf[n++] = '-';
    const uint16_t* d = nb->digits;
    int digits = 0;
    while (*d && n < 360) { buf[n++] = *d++; digits++; }
    if (!digits) buf[n++] = '0';
    int exp = nb->scale - digits;
    while (exp > 0) { buf[n++] = '0'; exp--; }
    buf[n] = 0;
    DECIMAL r;
    if (VarDecFromStr(buf, LOCALE_INVARIANT, 0, &r)) return 0;
    if (exp < 0) { DECIMAL p; VarDecFromI4(1, &p); for (int i = 0; i < -exp && i < 28; i++) { DECIMAL ten, t; VarDecFromI4(10, &ten); VarDecMul(&p, &ten, &t); p = t; } DECIMAL q; if (VarDecDiv(&r, &p, &q)) return 0; r = q; }
    *v = r;
    return 1;
}

/* ---- Enum ---- */
static uint64_t enum_raw(NrClass* u, const void* p)
{
    switch (u->byval_arg.type)
    {
    case 2: case 5: return *(const uint8_t*)p;
    case 4: return (uint64_t)(int64_t)*(const int8_t*)p;
    case 3: case 7: return *(const uint16_t*)p;
    case 6: return (uint64_t)(int64_t)*(const int16_t*)p;
    case 8: return (uint64_t)(int64_t)*(const int32_t*)p;
    case 9: return *(const uint32_t*)p;
    default: return *(const uint64_t*)p;
    }
}
static uint8_t Enum_GetEnumValuesAndNames(void* t, void** values, void** names)
{
    NrClass* k = nr_type_class(t);
    NrClass* u = k->elem;
    int n = 0;
    for (int i = 0; i < k->n_fields; i++) if ((k->fields[i].flags & 0x10) && k->fields[i].rva) n++;
    void* va = nr_newarr(nr_array_class(nr_class_by_name("System", "UInt64"), 1), n);
    void* na = nr_new_string_array(n);
    int j = 0;
    for (int i = 0; i < k->n_fields; i++)
    {
        const NrFieldDesc* f = &k->fields[i];
        if (!(f->flags & 0x10) || !f->rva) continue;
        ((uint64_t*)ARR(va)->vector)[j] = enum_raw(u, f->rva);
        ((void**)ARR(na)->vector)[j] = nr_str_utf8(f->name);
        j++;
    }
    *values = va; *names = na;
    return 0;   /* not sorted: the caller sorts by value */
}
static void* Enum_InternalBoxEnum(void* t, int64_t v) { return nr_box(nr_type_class(t), &v); }
static void* Enum_InternalGetUnderlyingType(void* t) { return nr_type_object(nr_type_class(t)->elem); }
static int32_t Enum_InternalCompareTo(void* a, void* b)
{
    NrClass* u = OBJ(a)->klass->elem;
    uint64_t x = enum_raw(u, (char*)a + sizeof(NrObj)), y = enum_raw(u, (char*)b + sizeof(NrObj));
    uint8_t ty = u->byval_arg.type;
    if (ty == 4 || ty == 6 || ty == 8 || ty == 0xa) return (int64_t)x < (int64_t)y ? -1 : (int64_t)x > (int64_t)y;
    return x < y ? -1 : x > y;
}

/* Enum.HasFlag: (this & flag) == flag on the underlying values */
static uint8_t Enum_InternalHasFlag(void* self, void* flag)
{
    NrClass* u = OBJ(self)->klass->elem;
    uint64_t x = enum_raw(u, (char*)self + sizeof(NrObj)), f = enum_raw(OBJ(flag)->klass->elem ? OBJ(flag)->klass->elem : u, (char*)flag + sizeof(NrObj));
    return (x & f) == f;
}

/* ---- MonoIO: paths arrive as char* (UTF-16, NUL-terminated) ---- */
static intptr_t MonoIO_Open4(const wchar_t* path, int32_t mode, int32_t access, int32_t share, int32_t options, int32_t* err)
{
    return MonoIO_Open(nr_str_from_utf16z((const uint16_t*)path), mode, access, share, options, err);
}
#define WPATH(p) nr_str_from_utf16z((const uint16_t*)(p))
static int32_t MonoIO_GetFileAttributes4(const wchar_t* p, int32_t* err) { DWORD a = GetFileAttributesW(p); *err = a == INVALID_FILE_ATTRIBUTES ? io_err() : 0; return (int32_t)a; }
static uint8_t MonoIO_SetFileAttributes4(const wchar_t* p, int32_t attrs, int32_t* err) { BOOL ok = SetFileAttributesW(p, (DWORD)attrs); *err = ok ? 0 : io_err(); return ok != 0; }
typedef struct MonoIOStat4 { int32_t attrs; int64_t length, ctime, atime, mtime; } MonoIOStat4;
static uint8_t MonoIO_GetFileStat4(const wchar_t* p, MonoIOStat4* st, int32_t* err)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) { *err = io_err(); memset(st, 0, sizeof *st); return 0; }
    st->attrs = (int32_t)d.dwFileAttributes;
    st->length = (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    st->ctime = ft_ticks(d.ftCreationTime) - TICKS_1601; st->atime = ft_ticks(d.ftLastAccessTime) - TICKS_1601; st->mtime = ft_ticks(d.ftLastWriteTime) - TICKS_1601;
    *err = 0;
    return 1;
}
static uint8_t MonoIO_CreateDirectory4(const wchar_t* p, int32_t* err) { BOOL ok = CreateDirectoryW(p, 0); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_RemoveDirectory4(const wchar_t* p, int32_t* err) { BOOL ok = RemoveDirectoryW(p); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_DeleteFile4(const wchar_t* p, int32_t* err) { BOOL ok = DeleteFileW(p); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_MoveFile4(const wchar_t* a, const wchar_t* b, int32_t* err) { BOOL ok = MoveFileW(a, b); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_CopyFile4(const wchar_t* a, const wchar_t* b, uint8_t ow, int32_t* err) { BOOL ok = CopyFileW(a, b, !ow); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_ReplaceFile4(const wchar_t* src, const wchar_t* dst, const wchar_t* bak, uint8_t ign, int32_t* err) { (void)ign; BOOL ok = ReplaceFileW(dst, src, bak, 0, 0, 0); *err = ok ? 0 : io_err(); return ok != 0; }
static uint8_t MonoIO_SetCurrentDirectory4(const wchar_t* p, int32_t* err) { BOOL ok = SetCurrentDirectoryW(p); *err = ok ? 0 : io_err(); return ok != 0; }
/* FindFirstFile(char* pattern, out string fileName, out int attrs, out int error) -> handle */
static intptr_t MonoIO_FindFirstFile(const wchar_t* pat, void** name, int32_t* attrs, int32_t* err)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { *name = 0; *attrs = 0; *err = io_err(); return -1; }
    *name = nr_str_utf16((uint16_t*)fd.cFileName, (int32_t)wcslen(fd.cFileName));
    *attrs = (int32_t)fd.dwFileAttributes; *err = 0;
    return (intptr_t)h;
}
static uint8_t MonoIO_FindNextFile(intptr_t h, void** name, int32_t* attrs, int32_t* err)
{
    WIN32_FIND_DATAW fd;
    if (!FindNextFileW((HANDLE)h, &fd)) { *name = 0; *attrs = 0; *err = io_err(); return 0; }
    *name = nr_str_utf16((uint16_t*)fd.cFileName, (int32_t)wcslen(fd.cFileName));
    *attrs = (int32_t)fd.dwFileAttributes; *err = 0;
    return 1;
}
static uint8_t MonoIO_FindCloseFile(intptr_t h) { return FindClose((HANDLE)h) != 0; }
static uint8_t MonoIO_RemapPath(void* path, void** out) { (void)path; *out = 0; return 0; }
static void MonoIO_DumpHandles(void) {}
static uint8_t MonoIO_DuplicateHandle(intptr_t sp, intptr_t sh, intptr_t tp, intptr_t* th, int32_t acc, int32_t inh, int32_t opt, int32_t* err)
{
    HANDLE out = 0;
    BOOL ok = DuplicateHandle((HANDLE)sp, (HANDLE)sh, (HANDLE)tp, &out, (DWORD)acc, inh, (DWORD)opt);
    *th = (intptr_t)out; *err = ok ? 0 : io_err();
    return ok != 0;
}

/* ---- culture: invariant only (CultureInfo falls back to it) ---- */
static void* CultureInfo_get_current_locale_name(void) { return 0; }
static void* CultureInfo_internal_get_cultures(uint8_t n, uint8_t s, uint8_t i) { (void)n; (void)s; (void)i; return nr_newarr(nr_array_class(nr_class_by_name("System.Globalization", "CultureInfo"), 1), 0); }
/* mono fills a named culture's time data from its native tables, which aren't
 * here; left empty, DateTime.ToString under that culture dereferenced null
 * LongTimes (BATTLETECH: StoryConstantsDef, so SimGameConstants never loaded).
 * ponytail: every named culture gets en-US time data; ship mono's culture
 * tables if a title formats times for another locale. */
static void* str_array(const char* const* v, int n)
{
    void* a = nr_newarr(nr_array_class(nr_k_string, 1), n);
    for (int i = 0; i < n; i++) ((void**)((NrArray*)a)->vector)[i] = nr_str_utf8(v[i]);
    return a;
}
static void CultureData_fill_culture_data(void* self, int32_t idx)
{
    (void)idx;
    NrClass* k = OBJ(self)->klass;
    static const char* const lt[] = { "h:mm:ss tt", "hh:mm:ss tt", "H:mm:ss", "HH:mm:ss" };
    static const char* const st[] = { "h:mm tt", "hh:mm tt", "H:mm", "HH:mm" };
    int32_t o;
#define SETREF(n, v) if ((o = foff(k, n)) > 0) FIELD(self, void*, o) = (v)
    SETREF("sAM1159", nr_str_utf8("AM"));
    SETREF("sPM2359", nr_str_utf8("PM"));
    SETREF("sTimeSeparator", nr_str_utf8(":"));
    SETREF("saLongTimes", str_array(lt, 4));
    SETREF("saShortTimes", str_array(st, 4));
#undef SETREF
    /* iFirstDayOfWeek, iFirstWeekOfYear: 0 (Sunday, FirstDay) as allocated */
}
static void CultureData_fill_number_data(void* nfi, int32_t idx) { (void)nfi; (void)idx; }
static uint8_t CalendarData_fill_calendar_data(void* self, void* name, int32_t id) { (void)self; (void)name; (void)id; return 0; }
static void CompareInfo_assign_sortkey4(void* self, void* key, void* src, int32_t opts) { CompareInfo_assign_sortkey(self, key, src, opts); }
static void Normalization_load_resource(intptr_t* a, intptr_t* b, intptr_t* c, intptr_t* d, intptr_t* e, intptr_t* f)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    nr_throw_notsupported("string normalization tables");
}

/* ---- AppDomain ---- */
static void* AppDomain_InternalGetProcessGuid(void* g) { static void* guid; if (!guid) { guid = g; nr_gchandle_new(g, 0); } return guid; }
static uint8_t AppDomain_false_i(int32_t id) { (void)id; return 0; }
static void AppDomain_void0(void) {}
static void AppDomain_void1(void* a) { (void)a; }
static void AppDomain_void_i(int32_t a) { (void)a; }
static void* AppDomain_InternalSetDomain(void* d) { return d; }
static void* AppDomain_InternalSetDomainByID(int32_t id) { (void)id; return AppDomain_getCurDomain(); }

/* ---- Marshal / GCHandle ---- */
static void* Marshal_GetDelegateForFunctionPointerInternal(intptr_t fn, void* t)
{
    NrClass* k = nr_type_class(t);
    void* d = nr_new(k);
    nr_delegate_init(d, 0, (void*)fn, 0);
    return d;
}
static intptr_t Marshal_GetFunctionPointerForDelegateInternal(void* d) { return (intptr_t)nr_delegate_to_fnptr(d); }
static void Marshal_Prelink(void* m) { (void)m; }
static int32_t Marshal_com_unsupported(intptr_t p) { (void)p; nr_throw_notsupported("COM interop"); }
static int32_t Marshal_GetHRForException_WinRT(void* e) { (void)e; return 0; }
static intptr_t Marshal_GetIUnknownForObjectInternal(void* o) { (void)o; nr_throw_notsupported("COM interop"); }
static int32_t Marshal_QueryInterfaceInternal(intptr_t p, void* iid, intptr_t* out) { (void)p; (void)iid; *out = 0; return (int32_t)0x80004002; }

/* ---- Delegate ---- */
static void* Delegate_AllocDelegateLike_internal(void* d)
{
    NrClass* k = OBJ(d)->klass;
    void* r = nr_new(k);
    memcpy((char*)r + sizeof(NrObj), (char*)d + sizeof(NrObj), (size_t)k->instance_size - sizeof(NrObj));
    ((NrDelegate*)r)->delegates = 0;
    return r;
}
static void* Delegate_GetVirtualMethod_internal(void* d)
{
    NrDelegate* dl = (NrDelegate*)d;
    return dl->method ? nr_method_object((const NrMethodDesc*)dl->method) : 0;
}

static void* ActivationServices_AllocateUninitializedClassInstance(void* t) { return nr_new(nr_type_class(t)); }
static void ActivationServices_EnableProxyActivation(void* t, uint8_t e) { (void)t; (void)e; }
static uint8_t RemotingServices_IsTransparentProxy(void* o) { (void)o; return 0; }
static void* TypedReference_MakeTypedReferenceInternal(void* target, void* fields) { (void)target; (void)fields; nr_throw_notsupported("TypedReference"); }

static void nr_register_corlib4_icalls(void)
{
    static const NrIcall t[] = {
        { "System.String::FastAllocateString", String_FastAllocateString },
        { "System.Threading.Monitor::try_enter_with_atomic_var", Monitor_try_enter_with_atomic_var },
        { "System.Threading.Interlocked::CompareExchange(int&,int,int,bool&)", IL_CmpXchg_i4b },
        { "System.Threading.Volatile::Read(bool&)", Vol_Read_u1 },
        { "System.Threading.Volatile::Read(int&)", VR_i4 },
        { "System.Threading.Volatile::Read(T&)", Vol_Read_T },
        { "System.Threading.Volatile::Write(int&,int)", VW_i4 },
        { "System.Threading.Volatile::Write(T&,T)", Vol_Write_T },
        { "System.Threading.Thread::ConstructInternalThread", Thread_ConstructInternalThread },
        { "System.Threading.Thread::GetCurrentThread", Thread_GetCurrentThread },
        { "System.Threading.Thread::SleepInternal", Thread_SleepInternal },
        { "System.Threading.Thread::YieldInternal", Thread_YieldInternal },
        { "System.Threading.Thread::SystemMaxStackStize", Thread_SystemMaxStackStize },
        { "System.Threading.Thread::JoinInternal", Thread_JoinInternal },
        { "System.Threading.Thread::SetPriorityNative", Thread_SetPriorityNative },
        { "System.Threading.Thread::GetPriorityNative", Thread_GetPriorityNative },
        { "System.Threading.Thread::GetAbortExceptionState", Thread_GetAbortExceptionState },
        { "System.Threading.Thread::ByteArrayToRootDomain", Thread_ByteArrayToDomain },
        { "System.Threading.Thread::ByteArrayToCurrentDomain", Thread_ByteArrayToDomain },
        { "System.Threading.Thread::InterruptInternal", Thread_InterruptInternal },
        { "System.Threading.Thread::VolatileRead(intptr&)", VR_ip },
        { "System.Threading.Thread::VolatileWrite(intptr&,intptr)", VW_ip },
        { "System.Threading.ThreadPool::RequestWorkerThread", ThreadPool_RequestWorkerThread },
        { "System.Threading.ThreadPool::InitializeVMTp", ThreadPool_InitializeVMTp },
        { "System.Threading.ThreadPool::IsThreadPoolHosted", ThreadPool_false },
        { "System.Threading.ThreadPool::NotifyWorkItemComplete", ThreadPool_true },
        { "System.Threading.ThreadPool::NotifyWorkItemProgressNative", ThreadPool_void },
        { "System.Threading.ThreadPool::ReportThreadStatus", ThreadPool_ReportThreadStatus },
        { "System.Threading.ThreadPool::SetMinThreadsNative", ThreadPool_SetThreads },
        { "System.Threading.ThreadPool::SetMaxThreadsNative", ThreadPool_SetThreads },
        { "System.Threading.ThreadPool::GetMinThreadsNative", ThreadPool_GetThreads },
        { "System.Threading.ThreadPool::GetMaxThreadsNative", ThreadPool_GetThreads },
        { "System.Threading.ThreadPool::GetAvailableThreadsNative", ThreadPool_GetThreads },
        { "System.Threading.ThreadPool::PostQueuedCompletionStatus", ThreadPool_PostQueuedCompletionStatus },
        { "System.Threading.ThreadPool::BindIOCompletionCallbackNative", ThreadPool_BindIO },
        { "System.Threading.Timer::GetTimeMonotonic", Timer_GetTimeMonotonic },
        { "System.Threading.NativeEventCalls::CreateEvent_internal", Event_Create4 },
        { "System.Threading.WaitHandle::Wait_internal", WaitHandle_Wait_internal },
        { "System.Threading.WaitHandle::SignalAndWait_Internal", WaitHandle_SignalAndWait4 },
        { "System.Threading.Mutex::CreateMutex_internal", Mutex_Create4 },
        { "System.Threading.Semaphore::CreateSemaphore_internal", Semaphore_Create4 },
        { "System.Threading.Semaphore::ReleaseSemaphore_internal", Semaphore_Release4 },
        { "System.GC::_SuppressFinalize", GC_SuppressFinalize },
        { "System.GC::_ReRegisterForFinalize", GC_ReRegisterForFinalize },
        { "System.GC::GetCollectionCount", GC_GetCollectionCount },
        { "System.GC::get_ephemeron_tombstone", GC_get_ephemeron_tombstone },
        { "System.GC::register_ephemeron_array", GC_register_ephemeron_array },
        { "System.GC::GetGeneration", GC_GetGeneration },
        { "System.GC::RecordPressure", GC_RecordPressure },
        { "System.GC::_Collect", GC_InternalCollect },
        { "System.GC::GetMaxGeneration", GC_get_MaxGeneration },
        { "System.DateTime::GetSystemTimeAsFileTime", DateTime_GetSystemTimeAsFileTime },
        { "System.CurrentSystemTimeZone::GetTimeZoneData", TimeZone_GetTimeZoneData4 },
        { "System.Environment::GetNewLine", Environment_GetNewLine },
        { "System.Environment::GetPageSize", Environment_GetPageSize },
        { "System.Environment::internalGetEnvironmentVariable_native", Environment_internalGetEnvironmentVariable_native },
        { "System.Environment::internalGetHome", Environment_internalGetHome },
        { "System.CLRConfig::CheckThrowUnobservedTaskExceptions", CLRConfig_false },
        { "System.Diagnostics.Process::ProcessName_internal", Process_ProcessName_internal },
        { "Microsoft.Win32.NativeMethods::GetCurrentProcess", NativeMethods_GetCurrentProcess },
        { "Microsoft.Win32.NativeMethods::GetCurrentProcessId", NativeMethods_GetCurrentProcessId },
        { "Microsoft.Win32.NativeMethods::GetExitCodeProcess", NativeMethods_GetExitCodeProcess },
        { "Microsoft.Win32.NativeMethods::CloseProcess", NativeMethods_CloseProcess },
        { "System.Diagnostics.Debugger::IsAttached_internal", Debugger_false },
        { "System.Diagnostics.Debugger::IsLogging", Debugger_false },
        { "System.Diagnostics.Debugger::Log", Debugger_Log },
        { "System.Diagnostics.DefaultTraceListener::WriteWindowsDebugString", DefaultTraceListener_WriteWindowsDebugString },
        { "Mono.SafeStringMarshal::StringToUtf8", SafeStringMarshal_StringToUtf8 },
        { "Mono.SafeStringMarshal::GFree", SafeStringMarshal_GFree },
        { "Mono.RuntimeGPtrArrayHandle::GPtrArrayFree", GPtrArray_Free },
        { "System.Text.EncodingHelper::InternalCodePage", Encoding_InternalCodePage },
        { "System.Buffer::InternalBlockCopy", Buffer_InternalBlockCopy },
        { "System.Buffer::_ByteLength", Buffer_ByteLength4 },
        { "System.Math::SplitFractionDouble", Math_SplitFractionDouble },
        { "System.Math::Round", Math_Round4 },
        { "System.Decimal::.ctor(double)", Decimal_ctor_r8 },
        { "System.Decimal::.ctor(single)", Decimal_ctor_r4 },
        { "System.Decimal::FCallAddSub", Decimal_FCallAddSub },
        { "System.Decimal::FCallMultiply", Decimal_FCallMultiply },
        { "System.Decimal::FCallDivide", Decimal_FCallDivide },
        { "System.Decimal::FCallCompare", Decimal_FCallCompare },
        { "System.Decimal::FCallFloor", Decimal_FCallFloor },
        { "System.Decimal::FCallTruncate", Decimal_FCallTruncate },
        { "System.Decimal::FCallRound", Decimal_FCallRound },
        { "System.Decimal::FCallToInt32", Decimal_FCallToInt32 },
        { "System.Decimal::ToDouble", Decimal_ToDouble },
        { "System.Decimal::ToSingle", Decimal_ToSingle },
        { "System.Decimal::GetHashCode", Decimal_GetHashCode },
        { "System.Number::NumberBufferToDouble", Number_NumberBufferToDouble },
        { "System.Number::NumberBufferToDecimal", Number_NumberBufferToDecimal },
        { "System.Enum::GetEnumValuesAndNames", Enum_GetEnumValuesAndNames },
        { "System.Enum::InternalBoxEnum", Enum_InternalBoxEnum },
        { "System.Enum::InternalGetUnderlyingType", Enum_InternalGetUnderlyingType },
        { "System.Enum::InternalCompareTo", Enum_InternalCompareTo },
        { "System.Enum::InternalHasFlag", Enum_InternalHasFlag },
        { "System.IO.MonoIO::Open", MonoIO_Open4 },
        { "System.IO.MonoIO::GetFileAttributes", MonoIO_GetFileAttributes4 },
        { "System.IO.MonoIO::SetFileAttributes", MonoIO_SetFileAttributes4 },
        { "System.IO.MonoIO::GetFileStat", MonoIO_GetFileStat4 },
        { "System.IO.MonoIO::CreateDirectory", MonoIO_CreateDirectory4 },
        { "System.IO.MonoIO::RemoveDirectory", MonoIO_RemoveDirectory4 },
        { "System.IO.MonoIO::DeleteFile", MonoIO_DeleteFile4 },
        { "System.IO.MonoIO::MoveFile", MonoIO_MoveFile4 },
        { "System.IO.MonoIO::CopyFile", MonoIO_CopyFile4 },
        { "System.IO.MonoIO::ReplaceFile", MonoIO_ReplaceFile4 },
        { "System.IO.MonoIO::SetCurrentDirectory", MonoIO_SetCurrentDirectory4 },
        { "System.IO.MonoIO::FindFirstFile", MonoIO_FindFirstFile },
        { "System.IO.MonoIO::FindNextFile", MonoIO_FindNextFile },
        { "System.IO.MonoIO::FindCloseFile", MonoIO_FindCloseFile },
        { "System.IO.MonoIO::RemapPath", MonoIO_RemapPath },
        { "System.IO.MonoIO::DumpHandles", MonoIO_DumpHandles },
        { "System.IO.MonoIO::DuplicateHandle", MonoIO_DuplicateHandle },
        { "System.Globalization.CultureInfo::get_current_locale_name", CultureInfo_get_current_locale_name },
        { "System.Globalization.CultureInfo::internal_get_cultures", CultureInfo_internal_get_cultures },
        { "System.Globalization.CultureData::fill_culture_data", CultureData_fill_culture_data },
        { "System.Globalization.CultureData::fill_number_data", CultureData_fill_number_data },
        { "System.Globalization.CalendarData::fill_calendar_data", CalendarData_fill_calendar_data },
        { "System.Globalization.CompareInfo::assign_sortkey", CompareInfo_assign_sortkey4 },
        { "System.Text.Normalization::load_normalization_resource", Normalization_load_resource },
        { "System.AppDomain::InternalGetProcessGuid", AppDomain_InternalGetProcessGuid },
        { "System.AppDomain::InternalIsFinalizingForUnload", AppDomain_false_i },
        { "System.AppDomain::InternalPopDomainRef", AppDomain_void0 },
        { "System.AppDomain::InternalPushDomainRef", AppDomain_void1 },
        { "System.AppDomain::InternalPushDomainRefByID", AppDomain_void_i },
        { "System.AppDomain::InternalSetDomain", AppDomain_InternalSetDomain },
        { "System.AppDomain::InternalSetDomainByID", AppDomain_InternalSetDomainByID },
        { "System.Runtime.InteropServices.Marshal::GetDelegateForFunctionPointerInternal", Marshal_GetDelegateForFunctionPointerInternal },
        { "System.Runtime.InteropServices.Marshal::GetFunctionPointerForDelegateInternal", Marshal_GetFunctionPointerForDelegateInternal },
        { "System.Runtime.InteropServices.Marshal::Prelink", Marshal_Prelink },
        { "System.Runtime.InteropServices.Marshal::AddRefInternal", Marshal_com_unsupported },
        { "System.Runtime.InteropServices.Marshal::ReleaseInternal", Marshal_com_unsupported },
        { "System.Runtime.InteropServices.Marshal::GetHRForException_WinRT", Marshal_GetHRForException_WinRT },
        { "System.Runtime.InteropServices.Marshal::GetIUnknownForObjectInternal", Marshal_GetIUnknownForObjectInternal },
        { "System.Runtime.InteropServices.Marshal::QueryInterfaceInternal", Marshal_QueryInterfaceInternal },
        { "System.Delegate::AllocDelegateLike_internal", Delegate_AllocDelegateLike_internal },
        { "System.Delegate::GetVirtualMethod_internal", Delegate_GetVirtualMethod_internal },
        { "System.Runtime.Remoting.Activation.ActivationServices::AllocateUninitializedClassInstance", ActivationServices_AllocateUninitializedClassInstance },
        { "System.Runtime.Remoting.Activation.ActivationServices::EnableProxyActivation", ActivationServices_EnableProxyActivation },
        { "System.Runtime.Remoting.RemotingServices::IsTransparentProxy", RemotingServices_IsTransparentProxy },
        { "System.TypedReference::MakeTypedReferenceInternal", TypedReference_MakeTypedReferenceInternal },
    };
    nr_register_icalls(t, (int)(sizeof t / sizeof t[0]));
}
#endif
