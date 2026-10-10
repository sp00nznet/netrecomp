/* nr_core.c: allocation, casts, dispatch, exceptions, strings, cctors,
 * delegates, the icall registry and P/Invoke binding.
 *
 * Exceptions are Win32 structured exceptions (code NR_EXC_CODE), so a managed
 * throw unwinds through native engine frames the way mono's did, and hardware
 * faults (null dereference, integer divide by zero) become managed exceptions
 * in the catch filter. See docs/runtime.md. */
#include "nrrt.h"
#include <stdarg.h>

extern IMAGE_DOS_HEADER __ImageBase;

NrClass *nr_k_object, *nr_k_string, *nr_k_array, *nr_k_monotype, *nr_k_void, *nr_k_int32,
    *nr_k_bool, *nr_k_char, *nr_k_byte, *nr_k_int64, *nr_k_double, *nr_k_single, *nr_k_intptr,
    *nr_k_exception, *nr_k_valuetype, *nr_k_enum, *nr_k_delegate, *nr_k_thread, *nr_k_objarr;
int nr_log_missing = 1;
void* (*nr_pinvoke_hook)(const char* lib, const char* entry);
void (*nr_unhandled_hook)(void* exc);

/* stderr, and the file named by NR_LOG when set (a Unity player has no console). */
void nr_log(const char* fmt, ...)
{
    static FILE* f;
    static int opened;
    if (!opened)
    {
        char p[MAX_PATH];
        opened = 1;
        if (GetEnvironmentVariableA("NR_LOG", p, MAX_PATH)) f = fopen(p, "a");
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
    if (f)
    {
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fflush(f);
    }
}

void nr_free(void* p) { free(p); }

/* ---- class lookup ---- */

NrClass* nr_class_in_image(const NrImage* img, const char* ns, const char* name)
{
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* k = nr_classes[i];
        if ((!img || k->image == img) && !strcmp(k->name, name) && !strcmp(k->ns, ns) && !k->nested_in
            && !(k->flags & (NR_ARRAY | NR_POINTER)) && !(k->flags & NR_GENERIC_INST))
            return k;
    }
    return 0;
}

NrClass* nr_class_by_name(const char* ns, const char* name) { return nr_class_in_image(0, ns, name); }

const NrFieldDesc* nr_field(NrClass* k, const char* name)
{
    for (; k; k = k->parent)
        for (int i = 0; i < k->n_fields; i++)
            if (!strcmp(k->fields[i].name, name)) return &k->fields[i];
    return 0;
}

const NrMethodDesc* nr_method(NrClass* k, const char* name, int nparams)
{
    for (; k; k = k->parent)
        for (int i = 0; i < k->n_methods; i++)
            if (!strcmp(k->methods[i].name, name) && (nparams < 0 || k->methods[i].n_params == nparams)) return &k->methods[i];
    return 0;
}

NrClass* nr_class_of(void* o) { return ((NrObj*)o)->klass; }
int nr_is_ref(NrClass* k) { return !(k->flags & NR_VALUETYPE) && !(k->flags & NR_POINTER); }
int32_t nr_value_size(NrClass* k) { return nr_is_ref(k) ? (int32_t)sizeof(void*) : k->value_size; }

/* T[] for a class: generated when the program uses it, else built here with
 * System.Array's vtable (no generic IList<T> on such arrays). */
static CRITICAL_SECTION arr_lock;
NrClass* nr_array_class(NrClass* elem, int rank)
{
    if (rank == 1 && elem->szarray) return elem->szarray;
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* k = nr_classes[i];
        if ((k->flags & NR_ARRAY) && k->elem == elem && k->rank == rank && (rank > 1 || k->byval_arg.type == 0x1d))
            return k;
    }
    EnterCriticalSection(&arr_lock);
    static struct DynArr { NrClass* elem; int rank; NrClass* k; struct DynArr* next; }* dyn;
    for (struct DynArr* d = dyn; d; d = d->next)
        if (d->elem == elem && d->rank == rank) { LeaveCriticalSection(&arr_lock); return d->k; }
    NrClass* k = (NrClass*)GC_MALLOC_UNCOLLECTABLE(sizeof(NrClass));
    *k = *nr_k_array;
    k->self = k;
    char* nm = (char*)malloc(strlen(elem->name) + 3 + rank);
    strcpy(nm, elem->name); strcat(nm, "[");
    for (int i = 1; i < rank; i++) strcat(nm, ",");
    strcat(nm, "]");
    k->name = nm; k->ns = elem->ns; k->image = elem->image;
    k->parent = nr_k_array; k->elem = elem; k->cast_class = (elem->flags & NR_ENUM) ? elem->elem : elem;
    k->rank = (uint16_t)rank; k->flags = NR_ARRAY | NR_SEALED | NR_CONSTRUCTED | ((nr_is_ref(elem) || (elem->flags & NR_HAS_REFS)) ? NR_HAS_REFS : 0);
    k->instance_size = 16; k->value_size = sizeof(void*); k->elem_size = nr_value_size(elem);
    static NrClass* sup[3];
    NrClass** s = (NrClass**)malloc(sizeof(NrClass*) * 3);
    s[0] = nr_k_object; s[1] = nr_k_array; s[2] = k;
    k->supers = s; k->depth = 2;
    k->byval_arg.klass = k; k->byval_arg.data = elem; k->byval_arg.type = rank == 1 ? 0x1d : 0x14; k->this_arg = k->byval_arg; k->this_arg.bits = 0x40;
    k->type_obj = 0; k->szarray = 0; k->fields = 0; k->n_fields = 0; k->methods = 0; k->n_methods = 0;
    (void)sup;
    struct DynArr* d = (struct DynArr*)malloc(sizeof *d);
    d->elem = elem; d->rank = rank; d->k = k; d->next = dyn; dyn = d;
    if (rank == 1) elem->szarray = k;
    LeaveCriticalSection(&arr_lock);
    return k;
}

/* ---- allocation ---- */

static void finalize_cb(void* obj, void* data);
static int32_t finalize_slot = -1;

static void* gc_alloc(NrClass* k, size_t size)
{
    void* o;
    if (k->flags & NR_HAS_REFS) o = GC_MALLOC(size);
    else { o = GC_MALLOC_ATOMIC(size); if (o) memset(o, 0, size); }
    if (!o) nr_throw_new("System", "OutOfMemoryException", "Out of memory");
    ((NrObj*)o)->klass = k;
    return o;
}

void* nr_new(NrClass* k)
{
    void* o = gc_alloc(k, (size_t)k->instance_size);
    if (k->flags & NR_HAS_FINALIZER) GC_REGISTER_FINALIZER_NO_ORDER(o, finalize_cb, 0, 0, 0);
    return o;
}

void* nr_newarr(NrClass* k, int32_t n)
{
    if (n < 0) nr_throw_overflow();
    uint64_t size = 16 + (uint64_t)k->elem_size * (uint32_t)n;
    if (size > 0x7fffffff) nr_throw_new("System", "OutOfMemoryException", "Array too large");
    NrArray* a = (NrArray*)gc_alloc(k, (size_t)size);
    a->max_length = (uint32_t)n;
    return a;
}

void* nr_new_mdarray(NrClass* k, int rank, const int32_t* lens)
{
    uint64_t total = 1;
    for (int i = 0; i < rank; i++) { if (lens[i] < 0) nr_throw_overflow(); total *= (uint32_t)lens[i]; }
    uint64_t data = 16 + (uint64_t)k->elem_size * total;
    data = (data + 7) & ~7ull;
    uint64_t size = data + sizeof(NrBounds) * rank;
    if (size > 0x7fffffff) nr_throw_new("System", "OutOfMemoryException", "Array too large");
    NrArray* a = (NrArray*)gc_alloc(k, (size_t)size);
    a->max_length = (uint32_t)total;
    a->bounds = (NrBounds*)((char*)a + data);
    for (int i = 0; i < rank; i++) { a->bounds[i].length = (uint32_t)lens[i]; a->bounds[i].lower_bound = 0; }
    return a;
}

void* nr_mdarray_addr(void* p, int rank, const int32_t* idx)
{
    NrArray* a = (NrArray*)p;
    if (!a) nr_throw_null();
    uint32_t off = 0;
    for (int i = 0; i < rank; i++)
    {
        int32_t j = idx[i] - a->bounds[i].lower_bound;
        if ((uint32_t)j >= a->bounds[i].length) nr_throw_range();
        off = off * a->bounds[i].length + (uint32_t)j;
    }
    return (char*)a->vector + (size_t)off * a->o.klass->elem_size;
}

void* nr_box(NrClass* k, const void* data)
{
    if (!(k->flags & NR_VALUETYPE)) return *(void**)data;
    void* o = gc_alloc(k, (size_t)k->instance_size);
    memcpy((char*)o + sizeof(NrObj), data, (size_t)k->value_size);
    return o;
}

void* nr_box_raw(NrClass* k, const void* raw) { return nr_box(k, raw); }

void* nr_box_nullable(NrClass* inner, const void* n, size_t has_off, size_t val_off)
{
    if (!((const uint8_t*)n)[has_off]) return 0;
    return nr_box(inner, (const char*)n + val_off);
}

void nr_unbox_nullable(void* o, NrClass* inner, void* out, size_t size, size_t has_off, size_t val_off)
{
    memset(out, 0, size);
    if (!o) return;
    memcpy((char*)out + val_off, nr_unbox(o, inner), (size_t)inner->value_size);
    ((uint8_t*)out)[has_off] = 1;
}

static NrClass* underlying(NrClass* k) { return (k->flags & NR_ENUM) ? k->elem : k; }

void* nr_unbox(void* o, NrClass* k)
{
    if (!o) nr_throw_null();
    NrClass* c = ((NrObj*)o)->klass;
    if (c != k && underlying(c) != underlying(k)) nr_throw_new("System", "InvalidCastException", "Specified cast is not valid.");
    return (char*)o + sizeof(NrObj);
}

/* ---- type tests ---- */

static int implements(NrClass* c, NrClass* iface)
{
    for (; c; c = c->parent)
        for (int i = 0; i < c->n_ifaces; i++)
            if (c->ifaces[i].iface == iface) return 1;
    return 0;
}

int nr_assignable(NrClass* to, NrClass* from)
{
    if (to == from) return 1;
    if (!to || !from) return 0;
    if (to->flags & NR_INTERFACE) return implements(from, to);
    if (to->flags & NR_ARRAY)
    {
        if (!(from->flags & NR_ARRAY) || from->rank != to->rank) return 0;
        if (to->byval_arg.type != from->byval_arg.type) return 0;
        NrClass *te = to->elem, *fe = from->elem;
        if (nr_is_ref(te) && nr_is_ref(fe)) return nr_assignable(te, fe);
        return underlying(te) == underlying(fe);
    }
    if (to->flags & NR_NULLABLE) return to->n_gargs && to->gargs[0] == from;
    return from->depth >= to->depth && from->supers && from->supers[to->depth] == to;
}

void* nr_isinst(void* o, NrClass* k)
{
    if (!o) return 0;
    NrClass* c = ((NrObj*)o)->klass;
    if (c == k) return o;
    return nr_assignable(k, c) ? o : 0;
}

void* nr_castclass(void* o, NrClass* k)
{
    if (!o || nr_isinst(o, k)) return o;
    char msg[512];
    NrClass* c = ((NrObj*)o)->klass;
    _snprintf(msg, sizeof msg, "Cannot cast from source type '%s.%s' to destination type '%s.%s'.", c->ns, c->name, k->ns, k->name);
    msg[sizeof msg - 1] = 0;
    nr_throw_new("System", "InvalidCastException", msg);
}

void* nr_iface(void* o, NrClass* iface, int k)
{
    if (!o) nr_throw_null();
    NrClass* c = ((NrObj*)o)->klass;
    for (int i = 0; i < c->n_ifaces; i++)
        if (c->ifaces[i].iface == iface)
        {
            int32_t off = c->ifaces[i].offset;
            void* fn = off >= 0 ? c->vtable[off + k] : 0;
            if (!fn) break;
            return fn;
        }
    char msg[512];
    _snprintf(msg, sizeof msg, "%s.%s does not implement %s.%s method %d", c->ns, c->name, iface->ns, iface->name, k);
    msg[sizeof msg - 1] = 0;
    nr_throw_new("System", "EntryPointNotFoundException", msg);
}

void* nr_gvm(void* o, const NrGvm* g)
{
    if (!o) nr_throw_null();
    NrClass* c = ((NrObj*)o)->klass;
    for (const NrGvmEntry* e = g->entries; e->klass; e++)
        if (e->klass == c) return e->fn;
    nr_throw_notsupported("generic virtual method not compiled for this receiver type");
}

void nr_stelem_ref(void* p, int32_t i, void* v)
{
    NrArray* a = (NrArray*)p;
    if (!a) nr_throw_null();
    if ((uint32_t)i >= a->max_length) nr_throw_range();
    if (v)
    {
        NrClass* e = a->o.klass->elem;
        if (e != nr_k_object && !nr_isinst(v, e)) nr_throw_new("System", "ArrayTypeMismatchException", "Attempted to access an element as a type incompatible with the array.");
    }
    ((void**)a->vector)[i] = v;
}

/* ---- exceptions ---- */

void* nr_exception(NrClass* k, const char* msg)
{
    void* e = nr_new(k);
    const NrMethodDesc* c = 0;
    for (int i = 0; i < k->n_methods; i++)
        if (!strcmp(k->methods[i].name, ".ctor") && k->methods[i].n_params == (msg ? 1 : 0) && k->methods[i].fn
            && (!msg || k->methods[i].params[0] == nr_k_string)) { c = &k->methods[i]; break; }
    if (c && msg) ((void (*)(void*, void*))c->fn)(e, nr_str_utf8(msg));
    else if (c) ((void (*)(void*))c->fn)(e);
    else if (msg)
    {
        const NrFieldDesc* f = nr_field(k, "message");
        if (f) *(void**)((char*)e + f->offset) = nr_str_utf8(msg);
    }
    return e;
}

void nr_throw_new(const char* ns, const char* name, const char* msg)
{
    NrClass* k = nr_class_by_name(ns, name);
    if (!k) { nr_log("netrecomp: exception class %s.%s missing (%s)\n", ns, name, msg ? msg : ""); k = nr_k_exception; }
    nr_throw(nr_exception(k, msg));
}

int nr_debug(void);
void nr_print_stack(int skip);
void nr_print_addr(void* addr);

void nr_throw(void* exc)
{
    if (!exc) nr_throw_null();
    if (nr_debug())
    {
        NrClass* k = ((NrObj*)exc)->klass;
        nr_log("netrecomp: throw %s.%s\n", k->ns, k->name);
        nr_print_stack(1);
    }
    ULONG_PTR a[1] = { (ULONG_PTR)exc };
    RaiseException(NR_EXC_CODE, 0, 1, a);
    for (;;) {}
}

void nr_throw_null(void) { nr_throw_new("System", "NullReferenceException", "Object reference not set to an instance of an object"); }
void nr_throw_range(void) { nr_throw_new("System", "IndexOutOfRangeException", "Array index is out of range."); }
void nr_throw_overflow(void) { nr_throw_new("System", "OverflowException", "Number overflow."); }
void nr_throw_divzero(void) { nr_throw_new("System", "DivideByZeroException", "Division by zero"); }
void nr_throw_arith(void) { nr_throw_new("System", "ArithmeticException", "Overflow or underflow in the arithmetic operation."); }
void nr_throw_notsupported(const char* what)
{
    if (nr_log_missing) nr_log("netrecomp: not supported: %s\n", what);
    nr_throw_new("System", "NotSupportedException", what);
}

/* A hardware fault seen by several filters on its way up must become one
 * managed object, not one per filter. */
static __declspec(thread) EXCEPTION_RECORD* last_rec;
static __declspec(thread) void* last_obj;

void* nr_current_exception_object(struct _EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode == NR_EXC_CODE && r->NumberParameters >= 1) return (void*)r->ExceptionInformation[0];
    if (last_rec == r && last_obj) return last_obj;
    const char* name = 0; const char* msg = 0;
    switch (r->ExceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        if (nr_debug()) { nr_log("netrecomp: access violation at %p (address %p)\n", r->ExceptionAddress, (void*)r->ExceptionInformation[1]); nr_print_addr(r->ExceptionAddress); }
        name = "NullReferenceException"; msg = "Object reference not set to an instance of an object"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: name = "DivideByZeroException"; msg = "Division by zero"; break;
    case EXCEPTION_INT_OVERFLOW: name = "ArithmeticException"; msg = "Overflow"; break;
    case EXCEPTION_STACK_OVERFLOW: return 0;
    default: return 0;
    }
    NrClass* k = nr_class_by_name("System", name);
    if (!k) return 0;
    last_obj = nr_exception(k, msg);
    last_rec = r;
    return last_obj;
}

int nr_catch(struct _EXCEPTION_POINTERS* ep, NrClass* k, void** ex)
{
    void* o = nr_current_exception_object(ep);
    if (!o) return EXCEPTION_CONTINUE_SEARCH;
    if (k && !nr_isinst(o, k)) return EXCEPTION_CONTINUE_SEARCH;
    *ex = o;
    return EXCEPTION_EXECUTE_HANDLER;
}

int nr_catch_any(struct _EXCEPTION_POINTERS* ep, NrClass* const* ks, int n, void** ex, int32_t* which)
{
    void* o = nr_current_exception_object(ep);
    if (!o) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < n; i++)
        if (nr_isinst(o, ks[i])) { *ex = o; *which = i; return EXCEPTION_EXECUTE_HANDLER; }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- strings ---- */

NrString* nr_str_alloc(int32_t len)
{
    if (len < 0) nr_throw_overflow();
    size_t size = 12 + 2 * ((size_t)len + 1);
    NrString* s = (NrString*)GC_MALLOC_ATOMIC(size);
    if (!s) nr_throw_new("System", "OutOfMemoryException", "Out of memory");
    memset(s, 0, size);
    s->o.klass = nr_k_string;
    s->length = len;
    return s;
}

NrString* nr_str_utf16(const uint16_t* p, int32_t len)
{
    NrString* s = nr_str_alloc(len);
    memcpy(s->chars, p, 2 * (size_t)len);
    return s;
}

NrString* nr_str_utf8(const char* p)
{
    if (!p) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, 0, 0);
    NrString* s = nr_str_alloc(n > 0 ? n - 1 : 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, p, -1, (LPWSTR)s->chars, n);
    return s;
}

char* nr_utf8(void* p)
{
    NrString* s = (NrString*)p;
    if (!s) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)s->chars, s->length, 0, 0, 0, 0);
    char* r = (char*)malloc((size_t)n + 1);
    WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)s->chars, s->length, r, n, 0, 0);
    r[n] = 0;
    return r;
}

char* nr_str_to_utf8(void* s) { return nr_utf8(s); }
const uint16_t* nr_str_chars(void* s) { return s ? ((NrString*)s)->chars : 0; }
void* nr_str_from_utf8(const char* s) { return nr_str_utf8(s); }
void* nr_str_from_utf16z(const uint16_t* s)
{
    if (!s) return 0;
    int32_t n = 0;
    while (s[n]) n++;
    return nr_str_utf16(s, n);
}

static CRITICAL_SECTION str_lock;
void* nr_strlit(int i)
{
    EnterCriticalSection(&str_lock);
    if (!nr_strtab[i])
    {
        const NrStrLit* l = &nr_strlits[i];
        size_t size = 12 + 2 * ((size_t)l->len + 1);
        NrString* s = (NrString*)GC_MALLOC_UNCOLLECTABLE(size);
        memset(s, 0, size);
        s->o.klass = nr_k_string;
        s->length = l->len;
        memcpy(s->chars, l->chars, 2 * (size_t)l->len);
        nr_strtab[i] = s;
    }
    LeaveCriticalSection(&str_lock);
    return nr_strtab[i];
}

void* nr_new_string_array(int n) { return nr_newarr(nr_array_class(nr_k_string, 1), n); }

/* ---- static constructors ----
 * ponytail: one recursive lock for every cctor. A cctor that waits on a thread
 * which needs another cctor deadlocks; per-class locks if a title hits it. */
static CRITICAL_SECTION cctor_lock;

void nr_run_cctor(NrClass* k)
{
    void* ex = 0;
    EnterCriticalSection(&cctor_lock);
    if (k->cctor_state != 0) { LeaveCriticalSection(&cctor_lock); return; }
    k->cctor_state = 1;
    if (k->cctor)
    {
        __try { k->cctor(); }
        __except (nr_catch(GetExceptionInformation(), 0, &ex)) { }
    }
    k->cctor_state = 2;
    LeaveCriticalSection(&cctor_lock);
    if (ex)
    {
        NrClass* tie = nr_class_by_name("System", "TypeInitializationException");
        void* e = nr_new(tie);
        const NrMethodDesc* c = 0;
        for (int i = 0; i < tie->n_methods; i++)
            if (!strcmp(tie->methods[i].name, ".ctor") && tie->methods[i].n_params == 2 && tie->methods[i].fn) c = &tie->methods[i];
        char nm[512];
        _snprintf(nm, sizeof nm, "%s%s%s", k->ns, *k->ns ? "." : "", k->name);
        nm[sizeof nm - 1] = 0;
        if (c) ((void (*)(void*, void*, void*))c->fn)(e, nr_str_utf8(nm), ex);
        nr_throw(e);
    }
}

/* ---- delegates ---- */

void nr_delegate_init(void* p, void* target, void* fn, const NrMethodDesc* m)
{
    NrDelegate* d = (NrDelegate*)p;
    d->target = target;
    d->method_ptr = fn;
    d->method = (void*)m;
}

/* A thunk slot of the delegate type's pool, bound to this delegate. The
 * binding holds the delegate alive (native code may call it any time).
 * ponytail: slots are never released; 32 per delegate type. */
static CRITICAL_SECTION rev_lock;
void* nr_delegate_to_fnptr(void* d)
{
    if (!d) return 0;
    NrClass* k = ((NrObj*)d)->klass;
    const NrRevPool* p = k->rev_pool;
    if (!p) nr_throw_notsupported("marshalling this delegate type to a native function pointer (no thunk pool)");
    EnterCriticalSection(&rev_lock);
    int free_slot = -1;
    for (int i = 0; i < p->n; i++)
    {
        if (!p->handles[i]) { if (free_slot < 0) free_slot = i; continue; }
        if (nr_gchandle_target(p->handles[i]) == d) { LeaveCriticalSection(&rev_lock); return p->fns[i]; }
    }
    if (free_slot < 0) { LeaveCriticalSection(&rev_lock); nr_throw_notsupported("delegate thunk pool exhausted"); }
    p->handles[free_slot] = nr_gchandle_new(d, 0);
    LeaveCriticalSection(&rev_lock);
    return p->fns[free_slot];
}

/* Entry from native code into a bound delegate: the thread may be new to us. */
void* nr_reverse_enter(uint32_t h)
{
    nr_current_thread_object();
    return nr_gchandle_target(h);
}

/* ---- finalizers ---- */

static void finalize_cb(void* obj, void* data)
{
    (void)data;
    NrClass* k = ((NrObj*)obj)->klass;
    if (finalize_slot < 0 || finalize_slot >= k->n_vtable || !k->vtable[finalize_slot]) return;
    void* ex;
    __try { ((void (*)(void*))k->vtable[finalize_slot])(obj); }
    __except (nr_catch(GetExceptionInformation(), 0, &ex)) { }
}

static HANDLE fin_event;
static void fin_notify(void) { SetEvent(fin_event); }
static DWORD WINAPI fin_thread(LPVOID p)
{
    (void)p;
    for (;;)
    {
        WaitForSingleObject(fin_event, INFINITE);
        GC_invoke_finalizers();
    }
}

/* ---- icall registry ---- */

typedef struct IcEnt { const char* name; const void* fn; struct IcEnt* next; } IcEnt;
static IcEnt* ic_tab[4096];
static CRITICAL_SECTION ic_lock;

static uint32_t hash(const char* s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

void nr_add_icall(const char* name, const void* fn)
{
    EnterCriticalSection(&ic_lock);
    uint32_t h = hash(name) & 4095;
    for (IcEnt* e = ic_tab[h]; e; e = e->next)
        if (!strcmp(e->name, name)) { e->fn = fn; LeaveCriticalSection(&ic_lock); return; }
    IcEnt* e = (IcEnt*)malloc(sizeof *e);
    e->name = _strdup(name); e->fn = fn; e->next = ic_tab[h]; ic_tab[h] = e;
    LeaveCriticalSection(&ic_lock);
}

void nr_register_icalls(const NrIcall* t, int n) { for (int i = 0; i < n; i++) nr_add_icall(t[i].name, t[i].fn); }

static const void* ic_find(const char* name)
{
    EnterCriticalSection(&ic_lock);
    const void* r = 0;
    for (IcEnt* e = ic_tab[hash(name) & 4095]; e; e = e->next)
        if (!strcmp(e->name, name)) { r = e->fn; break; }
    LeaveCriticalSection(&ic_lock);
    return r;
}

static __declspec(thread) const char* missing_name;
static void missing_icall(void)
{
    char msg[600];
    _snprintf(msg, sizeof msg, "internal call %s", missing_name ? missing_name : "?");
    msg[sizeof msg - 1] = 0;
    nr_throw_new("System", "MissingMethodException", msg);
}

void* nr_icall(const char* full, const char* name)
{
    const void* f = ic_find(full);
    if (!f) f = ic_find(name);
    if (!f)
    {
        if (nr_log_missing) nr_log("netrecomp: missing icall %s\n", full);
        missing_name = full;
        return (void*)missing_icall;
    }
    return (void*)f;
}

/* ---- P/Invoke ---- */

static void missing_entry(void) { nr_throw_new("System", "EntryPointNotFoundException", "P/Invoke entry point not found"); }

static void* find_export(HMODULE h, const char* entry)
{
    char name[600];
    void* f = (void*)GetProcAddress(h, entry);
    /* stdcall decoration: _name@N */
    for (int n = 0; n <= 64 && !f; n += 4)
    {
        _snprintf(name, sizeof name, "_%s@%d", entry, n);
        name[sizeof name - 1] = 0;
        f = (void*)GetProcAddress(h, name);
    }
    return f;
}

/* Library lookup order: the module itself for __Internal, then LoadLibrary
 * (with and without .dll), and only then the host's plugin hooks, which in a
 * Unity player log every miss. */
void* nr_pinvoke(const char* lib, const char* entry, const char* alt)
{
    HMODULE h = 0;
    char path[MAX_PATH * 2];
    if (!strcmp(lib, "__Internal")) h = GetModuleHandleA(0);
    if (!h) h = LoadLibraryA(lib);
    if (!h)
    {
        _snprintf(path, sizeof path, "%s.dll", lib);
        path[sizeof path - 1] = 0;
        h = LoadLibraryA(path);
    }
    void* f = 0;
    if (h)
    {
        f = find_export(h, entry);
        if (!f && alt) f = find_export(h, alt);
    }
    if (!f && nr_pinvoke_hook)
    {
        f = nr_pinvoke_hook(lib, entry);
        if (!f && alt) f = nr_pinvoke_hook(lib, alt);
    }
    if (!f)
    {
        if (nr_log_missing) nr_log("netrecomp: P/Invoke %s!%s not found\n", lib, entry);
        return (void*)missing_entry;
    }
    return f;
}

/* ---- GC handles ----
 * ponytail: a growable table under one lock; slots are reused via a free list. */
static void** gch_tab;
static uint8_t* gch_kind;          /* 0 free, 1 normal, 2 weak (hidden pointer + disappearing link) */
static uint32_t gch_cap, gch_hint = 1;
static CRITICAL_SECTION gch_lock;

static uint32_t gch_alloc(void)
{
    for (uint32_t i = gch_hint; i < gch_cap; i++) if (!gch_kind[i]) { gch_hint = i + 1; return i; }
    for (uint32_t i = 1; i < gch_hint && i < gch_cap; i++) if (!gch_kind[i]) { gch_hint = i + 1; return i; }
    uint32_t oc = gch_cap, nc = oc ? oc * 2 : 1024;
    void** nt = (void**)GC_MALLOC_UNCOLLECTABLE(nc * sizeof(void*));
    uint8_t* nk = (uint8_t*)calloc(nc, 1);
    if (oc)
    {
        memcpy(nt, gch_tab, oc * sizeof(void*));
        memcpy(nk, gch_kind, oc);
        for (uint32_t j = 1; j < oc; j++)
            if (gch_kind[j] == 2)
            {
                GC_unregister_disappearing_link(&gch_tab[j]);
                if (nt[j]) GC_GENERAL_REGISTER_DISAPPEARING_LINK(&nt[j], GC_REVEAL_POINTER((GC_hidden_pointer)nt[j]));
            }
        GC_FREE(gch_tab);
        free(gch_kind);
    }
    gch_tab = nt; gch_kind = nk; gch_cap = nc;
    gch_hint = (oc ? oc : 1) + 1;
    return oc ? oc : 1;
}

uint32_t nr_gchandle_new(void* o, int pinned)
{
    (void)pinned;   /* nothing moves: the collector is non-moving */
    EnterCriticalSection(&gch_lock);
    uint32_t i = gch_alloc();
    gch_tab[i] = o; gch_kind[i] = 1;
    LeaveCriticalSection(&gch_lock);
    return i << 3 | 1;
}

uint32_t nr_gchandle_new_weak(void* o)
{
    EnterCriticalSection(&gch_lock);
    uint32_t i = gch_alloc();
    gch_kind[i] = 2;
    /* the table is scanned memory: a weak slot holds a hidden pointer so it
     * doesn't keep its object alive */
    gch_tab[i] = o ? (void*)GC_HIDE_POINTER(o) : 0;
    if (o) GC_GENERAL_REGISTER_DISAPPEARING_LINK(&gch_tab[i], o);
    LeaveCriticalSection(&gch_lock);
    return i << 3 | 2;
}

static void* weak_get(void* p) { return *(void**)p ? (void*)GC_REVEAL_POINTER(*(GC_hidden_pointer*)p) : 0; }

void* nr_gchandle_target(uint32_t h)
{
    uint32_t i = h >> 3;
    if (!h || i >= gch_cap) return 0;
    if (gch_kind[i] == 2) return GC_call_with_alloc_lock(weak_get, &gch_tab[i]);
    return gch_tab[i];
}

void nr_gchandle_free(uint32_t h)
{
    uint32_t i = h >> 3;
    if (!h || i >= gch_cap) return;
    EnterCriticalSection(&gch_lock);
    if (gch_kind[i] == 2) GC_unregister_disappearing_link(&gch_tab[i]);
    gch_kind[i] = 0;
    gch_tab[i] = 0;
    LeaveCriticalSection(&gch_lock);
}

/* ---- types as objects ---- */

static CRITICAL_SECTION type_lock;

void* nr_type_object(NrClass* k)
{
    if (!k) return 0;
    if (k->type_obj) return k->type_obj;
    EnterCriticalSection(&type_lock);
    if (!k->type_obj)
    {
        void* t = GC_MALLOC_UNCOLLECTABLE((size_t)nr_k_monotype->instance_size);
        memset(t, 0, (size_t)nr_k_monotype->instance_size);
        ((NrObj*)t)->klass = nr_k_monotype;
        const NrFieldDesc* f = nr_field(nr_k_monotype, "_impl");
        if (f) *(NrType**)((char*)t + f->offset) = &k->byval_arg;
        k->type_obj = t;
    }
    LeaveCriticalSection(&type_lock);
    return k->type_obj;
}

NrType* nr_class_type(NrClass* k) { return &k->byval_arg; }

NrClass* nr_type_class(void* t)
{
    if (!t) return 0;
    static int32_t off = -1;
    if (off < 0) off = nr_field(nr_k_monotype, "_impl")->offset;
    NrType* ty = *(NrType**)((char*)t + off);
    return ty ? ty->klass : 0;
}

int nr_type_byref(void* t)
{
    const NrFieldDesc* f = nr_field(nr_k_monotype, "_impl");
    NrType* ty = *(NrType**)((char*)t + f->offset);
    return ty ? NR_TYPE_BYREF(ty) : 0;
}

/* ---- reflection invoke ---- */

void* nr_virtual_desc(const NrMethodDesc* m, void* obj, const NrMethodDesc** out)
{
    *out = m;
    if (!obj || m->slot < 0) return m->fn;
    NrClass* c = ((NrObj*)obj)->klass;
    if (m->klass->flags & NR_INTERFACE)
    {
        int idx = m->slot;
        for (int i = 0; i < c->n_ifaces; i++)
            if (c->ifaces[i].iface == m->klass && c->ifaces[i].offset >= 0) return c->vtable[c->ifaces[i].offset + idx];
        return m->fn;
    }
    if (m->slot < c->n_vtable && c->vtable[m->slot]) return c->vtable[m->slot];
    return m->fn;
}

void* nr_invoke(const NrMethodDesc* m, void* obj, void** args, void** exc)
{
    uint64_t raw[16];
    void* ret = 0;
    if (exc) *exc = 0;
    if (!m->fn || !m->invoke)
    {
        char msg[512];
        snprintf(msg, sizeof msg, "%s.%s::%s (not compiled)", m->klass->ns, m->klass->name, m->name);
        void* e = nr_exception(nr_class_by_name("System", "MissingMethodException"), msg);
        if (exc) { *exc = e; return 0; }
        nr_throw(e);
    }
    void* volatile e = 0;
    void* boxed_this = obj;
    if (obj && (m->klass->flags & NR_VALUETYPE) && ((NrObj*)obj)->klass == m->klass && !(m->flags & 0x10))
        obj = (char*)obj + sizeof(NrObj);   /* boxed receiver for a value-type method */
    __try
    {
        m->invoke(m->fn, obj, args, raw);
        NrClass* rk = m->ret;
        if (rk && rk != nr_k_void)
            ret = nr_is_ref(rk) ? *(void**)raw : nr_box(rk, raw);
    }
    __except (nr_catch(GetExceptionInformation(), 0, (void**)&e)) { }
    (void)boxed_this;
    if (e)
    {
        if (exc) { *exc = e; return 0; }
        nr_throw(e);
    }
    return ret;
}

/* ---- roots and init ---- */

static void add_module_roots(HMODULE mod)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)mod + dos->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (s->Characteristics & IMAGE_SCN_MEM_WRITE)
        {
            char* b = (char*)mod + s->VirtualAddress;
            GC_add_roots(b, b + s->Misc.VirtualSize);
        }
}

void nr_register_corlib_icalls(void);
int nr_debug(void);

static NrClass* need(const char* ns, const char* name)
{
    NrClass* k = nr_class_in_image(0, ns, name);
    if (!k) { nr_log("netrecomp: runtime class %s.%s missing from the build\n", ns, name); }
    return k;
}

void nr_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    nr_debug();
    InitializeCriticalSection(&arr_lock);
    InitializeCriticalSection(&str_lock);
    InitializeCriticalSection(&cctor_lock);
    InitializeCriticalSection(&ic_lock);
    InitializeCriticalSection(&gch_lock);
    InitializeCriticalSection(&type_lock);
    InitializeCriticalSection(&rev_lock);
    GC_set_no_dls(1);
    GC_set_finalize_on_demand(1);
    GC_INIT();
    GC_allow_register_threads();
    add_module_roots((HMODULE)&__ImageBase);
    fin_event = CreateEventA(0, FALSE, FALSE, 0);
    GC_set_finalizer_notifier(fin_notify);
    CreateThread(0, 0, fin_thread, 0, 0, 0);

    nr_k_object = need("System", "Object");
    nr_k_string = need("System", "String");
    nr_k_array = need("System", "Array");
    nr_k_monotype = need("System", "MonoType");
    nr_k_void = need("System", "Void");
    nr_k_int32 = need("System", "Int32");
    nr_k_bool = need("System", "Boolean");
    nr_k_char = need("System", "Char");
    nr_k_byte = need("System", "Byte");
    nr_k_int64 = need("System", "Int64");
    nr_k_double = need("System", "Double");
    nr_k_single = need("System", "Single");
    nr_k_intptr = need("System", "IntPtr");
    nr_k_exception = need("System", "Exception");
    nr_k_valuetype = need("System", "ValueType");
    nr_k_enum = need("System", "Enum");
    nr_k_delegate = need("System", "Delegate");
    nr_k_thread = need("System.Threading", "Thread");
    const NrMethodDesc* fin = nr_method(nr_k_object, "Finalize", 0);
    if (fin) finalize_slot = fin->slot;
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* k = nr_classes[i];
        if ((k->flags & NR_ARRAY) && k->rank == 1 && k->byval_arg.type == 0x1d && k->elem) k->elem->szarray = k;
    }
    nr_k_objarr = nr_array_class(nr_k_object, 1);
    nr_register_corlib_icalls();
    nr_thread_attach();
}

/* ---- NR_DEBUG: symbolized native stacks for throws and faults ---- */
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static int dbg_on = -1;
static CRITICAL_SECTION dbg_lock;
void nr_print_addr(void* addr);

/* "module+offset" for an address: what llvm-objdump --start-address wants. */
static void mod_off(void* a, char* out, size_t n)
{
    HMODULE m = 0;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m))
        GetModuleFileNameA(m, path, MAX_PATH);
    const char* b = strrchr(path, '\\');
    _snprintf(out, n, "%s+0x%x", b ? b + 1 : path, (unsigned)((uintptr_t)a - (uintptr_t)m));
    out[n - 1] = 0;
}

/* Faults outside the runtime's own code (the engine, a driver) with registers
 * and every stack word that points into a module. Managed null references
 * fault inside mono.dll and are expected; those only get a line. */
static LONG CALLBACK fault_veh(EXCEPTION_POINTERS* ep)
{
    static volatile LONG count;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && r->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION
        && r->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO && r->ExceptionCode != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE self = (HMODULE)&__ImageBase, at = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)r->ExceptionAddress, &at);
    if (at == self) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&count) > 8) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    char where[300];
    mod_off(r->ExceptionAddress, where, sizeof where);
    nr_log("netrecomp: fault 0x%08lx at %s (data %p) thread %lu\n", r->ExceptionCode, where,
           r->NumberParameters > 1 ? (void*)r->ExceptionInformation[1] : 0, GetCurrentThreadId());
    nr_log("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx\n",
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    uintptr_t* sp = (uintptr_t*)c->Esp;
    for (int i = 0; i < 400; i++)
    {
        uintptr_t v;
        if (IsBadReadPtr(sp + i, 4)) break;
        v = sp[i];
        HMODULE m = 0;
        if (v > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &m) && m)
        {
            char buf[300];
            mod_off((void*)v, buf, sizeof buf);
            nr_log("  [esp+%03x] %s\n", i * 4, buf);
            if (m == self) nr_print_addr((void*)v);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int nr_debug(void)
{
    if (dbg_on < 0)
    {
        dbg_on = GetEnvironmentVariableA("NR_DEBUG", 0, 0) > 0;
        if (dbg_on)
        {
            InitializeCriticalSection(&dbg_lock);
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            SymInitialize(GetCurrentProcess(), 0, TRUE);
            AddVectoredExceptionHandler(1, fault_veh);
        }
    }
    return dbg_on;
}

void nr_print_addr(void* addr)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 ln = { sizeof ln };
    DWORD ld = 0;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)addr, &disp, si))
    {
        if (SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)(uintptr_t)addr, &ld, &ln))
            nr_log("    %s+0x%llx (%s:%lu)\n", si->Name, disp, ln.FileName, ln.LineNumber);
        else nr_log("    %s+0x%llx\n", si->Name, disp);
    }
    else nr_log("    %p\n", addr);
}

void nr_print_stack(int skip)
{
    if (!nr_debug()) return;
    void* frames[48];
    USHORT n = CaptureStackBackTrace((DWORD)skip + 1, 48, frames, 0);
    EnterCriticalSection(&dbg_lock);
    for (USHORT i = 0; i < n; i++) nr_print_addr(frames[i]);
    LeaveCriticalSection(&dbg_lock);
}

/* ---- async delegate invocation ---- */

void* nr_alloc_root(size_t n) { return GC_MALLOC_UNCOLLECTABLE(n); }

typedef struct AsyncJob { void (*fn)(void*); void* arg; } AsyncJob;
static DWORD WINAPI async_main(LPVOID p)
{
    AsyncJob j = *(AsyncJob*)p;
    free(p);
    void* ex = 0;
    nr_current_thread_object();
    __try { j.fn(j.arg); }
    __except (nr_catch(GetExceptionInformation(), 0, &ex))
    {
        NrClass* k = ((NrObj*)ex)->klass;
        nr_log("netrecomp: unhandled exception %s.%s in an async delegate\n", k->ns, k->name);
    }
    GC_FREE(j.arg);
    return 0;
}

/* ponytail: a thread per job (CreateThread is GC_CreateThread here); a real
 * pool if a title queues work at a rate where thread creation shows. */
void nr_async_start(void (*fn)(void*), void* arg)
{
    AsyncJob* j = (AsyncJob*)malloc(sizeof *j);
    j->fn = fn; j->arg = arg;
    HANDLE h = CreateThread(0, 0, async_main, j, 0, 0);
    if (h) CloseHandle(h);
}

void nr_async_callback(void* cb)
{
    const NrMethodDesc* inv = nr_method(((NrObj*)cb)->klass, "Invoke", 1);
    void* args[1] = { 0 };
    void* ex = 0;
    if (inv) nr_invoke(inv, cb, args, &ex);
}

/* ---- native calls whatever their convention ----
 * Copies n dwords of arguments to the stack, calls fn, and restores the stack
 * pointer from the frame, so a cdecl callee (which leaves its arguments) and
 * a stdcall one (which pops them) both come back right. The integer result is
 * edx:eax; the _fp variant returns the x87 st(0) the callee left. */
__declspec(naked) int64_t nr_native_call(void* fn, const uint32_t* args, int32_t n)
{
    __asm {
        push ebp
        mov ebp, esp
        push esi
        push edi
        mov ecx, [ebp + 16]
        mov esi, [ebp + 12]
        lea eax, [ecx * 4]
        sub esp, eax
        mov edi, esp
        cld
        rep movsd
        call dword ptr [ebp + 8]
        lea esp, [ebp - 8]
        pop edi
        pop esi
        pop ebp
        ret
    }
}

__declspec(naked) double nr_native_call_fp(void* fn, const uint32_t* args, int32_t n)
{
    __asm {
        push ebp
        mov ebp, esp
        push esi
        push edi
        mov ecx, [ebp + 16]
        mov esi, [ebp + 12]
        lea eax, [ecx * 4]
        sub esp, eax
        mov edi, esp
        cld
        rep movsd
        call dword ptr [ebp + 8]
        lea esp, [ebp - 8]
        pop edi
        pop esi
        pop ebp
        ret
    }
}
