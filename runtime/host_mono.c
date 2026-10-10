/* host_mono.c: the mono embedding API, so the build can stand in for a Unity 4
 * player's Mono/mono.dll. The player resolves exactly these 181 exports by
 * name at start-up (and names any that are missing).
 *
 * Handle types map onto runtime records:
 *   MonoDomain*            a static NrDomain (one root, any child is the same world)
 *   MonoImage*, MonoAssembly*  NrImage* (the same pointer serves as both)
 *   MonoClass, MonoVTable*   NrClass*
 *   MonoType*              NrType* (&klass->byval_arg, or this_arg for byref)
 *   MonoMethod, MonoMethodSignature*  NrMethodDesc*
 *   MonoClassField*        NrFieldDesc*
 *   MonoProperty*          NrProp* (synthesized, see nr_reflect.c)
 *   MonoObject*, MonoString*, ...  the objects themselves (mono layout)
 * See docs/unity-host.md. */
#include "nrrt.h"
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#define API __declspec(dllexport)
typedef int gboolean;

typedef struct NrDomain { int32_t id; const char* name; } NrDomain;
static NrDomain root_domain = { 1, "Unity Root Domain" }, child_domain = { 2, "Unity Child Domain" };
static NrDomain* cur_domain = &root_domain;


static void (*unhandled_handler)(void* exc);

/* NR_TRACE=1: every API call, by name, to the log */
static int trace_on = -1;
#define TR(n) do { if (trace_on < 0) trace_on = GetEnvironmentVariableA("NR_TRACE", 0, 0) > 0; if (trace_on) nr_log("[mono] %s\n", n); } while (0)
static const char* (*find_plugin_cb)(const char*);
int nr_debug(void);

/* ---- lookups built once ---- */

typedef struct TokEnt { NrImage* img; uint32_t tok; NrClass* k; } TokEnt;
static TokEnt* tok_tab;
static int tok_n;
static int tok_cmp(const void* a, const void* b)
{
    const TokEnt *x = (const TokEnt*)a, *y = (const TokEnt*)b;
    if (x->img != y->img) return (uintptr_t)x->img < (uintptr_t)y->img ? -1 : 1;
    return x->tok < y->tok ? -1 : x->tok > y->tok;
}
static void build_tokens(void)
{
    tok_tab = (TokEnt*)malloc(sizeof(TokEnt) * (size_t)nr_n_classes);
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* k = nr_classes[i];
        if (k->flags & (NR_ARRAY | NR_POINTER | NR_GENERIC_INST) || !k->token || !k->image) continue;
        tok_tab[tok_n].img = k->image; tok_tab[tok_n].tok = k->token; tok_tab[tok_n].k = k; tok_n++;
    }
    qsort(tok_tab, (size_t)tok_n, sizeof(TokEnt), tok_cmp);
}

void* nr_pinvoke_fallback(const char* lib, const char* entry);
void nr_report_unhandled(void* exc);

static void ensure_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    nr_pinvoke_hook = nr_pinvoke_fallback;
    nr_unhandled_hook = nr_report_unhandled;
    nr_init();
    build_tokens();
}

/* ---- runtime set-up: mostly accepted and ignored ---- */

API NrDomain* mono_jit_init(const char* file) { TR("mono_jit_init"); (void)file; ensure_init(); return &root_domain; }
void nr_input_start(void);
API NrDomain* mono_jit_init_version(const char* name, const char* ver) { TR("mono_jit_init_version"); (void)name; (void)ver; ensure_init(); nr_input_start(); return &root_domain; }
API void mono_jit_cleanup(NrDomain* d) { TR("mono_jit_cleanup"); (void)d; }
API int mono_jit_exec(NrDomain* d, NrImage* a, int argc, char** argv) { TR("mono_jit_exec"); (void)d; (void)a; (void)argc; (void)argv; return 0; }
API void mono_jit_parse_options(int argc, char** argv) { TR("mono_jit_parse_options"); (void)argc; (void)argv; }
API void mono_set_dirs(const char* a, const char* c) { TR("mono_set_dirs"); (void)a; (void)c; }
API void mono_set_assemblies_path(const char* p) { TR("mono_set_assemblies_path"); (void)p; }
API void mono_config_parse(const char* f) { TR("mono_config_parse"); (void)f; }
API void mono_set_signal_chaining(gboolean b) { TR("mono_set_signal_chaining"); (void)b; }
API void mono_set_commandline_arguments(int argc, const char** argv, const char* base) { TR("mono_set_commandline_arguments"); (void)argc; (void)argv; (void)base; }
API void mono_debug_init(int fmt) { TR("mono_debug_init"); (void)fmt; }
API void mono_debug_open_image_from_memory(NrImage* img, const char* raw, int size) { TR("mono_debug_open_image_from_memory"); (void)img; (void)raw; (void)size; }
API uint32_t mono_parse_default_optimizations(const char* p) { TR("mono_parse_default_optimizations"); (void)p; return 0; }
API void mono_set_defaults(int verbose, uint32_t opts) { TR("mono_set_defaults"); (void)verbose; (void)opts; }
API void mono_trace_set_level_string(const char* s) { TR("mono_trace_set_level_string"); (void)s; }
API void mono_trace_set_mask_string(const char* s) { TR("mono_trace_set_mask_string"); (void)s; }
API void mono_verifier_set_mode(int m) { TR("mono_verifier_set_mode"); (void)m; }
API void mono_security_set_mode(int m) { TR("mono_security_set_mode"); (void)m; }
API void mono_security_enable_core_clr(void) { TR("mono_security_enable_core_clr");}
API void mono_security_set_core_clr_platform_callback(void* cb) { TR("mono_security_set_core_clr_platform_callback"); (void)cb; }
API void mono_unity_set_embeddinghostname(const char* n) { TR("mono_unity_set_embeddinghostname"); (void)n; }
API void mono_unity_socket_security_enabled_set(gboolean b) { TR("mono_unity_socket_security_enabled_set"); (void)b; }
API void mono_runtime_unhandled_exception_policy_set(int p) { TR("mono_runtime_unhandled_exception_policy_set"); (void)p; }
API void mono_set_ignore_version_and_key_when_finding_assemblies_already_loaded(gboolean b) { TR("mono_set_ignore_version_and_key_when_finding_assemblies_already_loaded"); (void)b; }
API void mono_unity_set_unhandled_exception_handler(void (*fn)(void*)) { TR("mono_unity_set_unhandled_exception_handler"); unhandled_handler = fn; }
API LONG mono_unity_seh_handler(EXCEPTION_POINTERS* ep) { TR("mono_unity_seh_handler"); (void)ep; return EXCEPTION_CONTINUE_SEARCH; }
API void mono_set_find_plugin_callback(const char* (*cb)(const char*)) { TR("mono_set_find_plugin_callback"); find_plugin_cb = cb; }

/* P/Invoke fallbacks Unity registers (resolving __Internal into the player) */
typedef void* (*DlLoad)(const char* name, int flags, char** err, void* user);
typedef void* (*DlSym)(void* handle, const char* name, char** err, void* user);
typedef struct DlFallback { DlLoad load; DlSym sym; void* close; void* user; } DlFallback;
static DlFallback dl_fallback;
API void* mono_dl_fallback_register(DlLoad load, DlSym sym, void* close, void* user)
{
    TR("mono_dl_fallback_register");
    dl_fallback.load = load; dl_fallback.sym = sym; dl_fallback.close = close; dl_fallback.user = user;
    return &dl_fallback;
}
API void mono_dl_fallback_unregister(void* h) { TR("mono_dl_fallback_unregister"); (void)h; memset(&dl_fallback, 0, sizeof dl_fallback); }

void* nr_pinvoke_fallback(const char* lib, const char* entry)
{
    if (find_plugin_cb)
    {
        const char* p = find_plugin_cb(lib);
        if (p && *p)
        {
            HMODULE h = LoadLibraryA(p);
            if (h) { void* f = (void*)GetProcAddress(h, entry); if (f) return f; }
        }
    }
    if (dl_fallback.load)
    {
        char* err = 0;
        void* h = dl_fallback.load(lib, 0, &err, dl_fallback.user);
        if (h && dl_fallback.sym) return dl_fallback.sym(h, entry, &err, dl_fallback.user);
    }
    return 0;
}

/* Unity's engine calls, by "Namespace.Type::Method" */
API void mono_add_internal_call(const char* name, const void* fn)
{
    TR("mono_add_internal_call");
    if (trace_on > 0) nr_log("[mono]   icall %s = exe+0x%x\n", name, (unsigned)((const char*)fn - (const char*)GetModuleHandleA(0)));
    ensure_init(); nr_add_icall(name, fn);
}

/* ---- domains ---- */

API NrDomain* mono_domain_get(void) { TR("mono_domain_get"); return cur_domain; }
API NrDomain* mono_get_root_domain(void) { TR("mono_get_root_domain"); return &root_domain; }
API gboolean mono_domain_set(NrDomain* d, gboolean force) { TR("mono_domain_set"); (void)force; if (d) cur_domain = d; return 1; }
API int32_t mono_domain_get_id(NrDomain* d) { TR("mono_domain_get_id"); return d ? d->id : 0; }
API NrDomain* mono_domain_create_appdomain(char* name, char* config) { TR("mono_domain_create_appdomain"); (void)name; (void)config; return &child_domain; }
API void mono_domain_unload(NrDomain* d) { TR("mono_domain_unload"); (void)d; }
API gboolean mono_domain_finalize(NrDomain* d, uint32_t timeout) { TR("mono_domain_finalize"); (void)d; (void)timeout; return 1; }

/* ---- images and assemblies ---- */

static NrImage* image_by_file(const char* path)
{
    if (!path) return 0;
    const char* base = path;
    for (const char* p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    for (int i = 0; i < nr_n_images; i++)
    {
        NrImage* img = nr_images[i];
        if (!_stricmp(img->file, base) || !_stricmp(img->name, base)) return img;
        size_t n = strlen(img->name);
        if (!_strnicmp(img->name, base, n) && (!_stricmp(base + n, ".dll") || !_stricmp(base + n, ".exe"))) return img;
    }
    if (nr_debug()) nr_log("netrecomp: no compiled image for %s\n", path);
    return 0;
}

/* The build must match the game's assemblies: hash what Unity hands us. */
static void check_hash(NrImage* img, const void* data, uint32_t len)
{
    if (!img || !img->sha256 || !data) return;
    BCRYPT_ALG_HANDLE alg;
    uint8_t h[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, 0)) return;
    BCryptHash(alg, 0, 0, (PUCHAR)data, len, h, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", h[i]);
    if (strcmp(hex, img->sha256))
        nr_log("netrecomp: %s differs from the assembly this mono.dll was built from; rebuild for this copy of the game\n", img->file);
}

API NrImage* mono_image_open_from_data_with_name(char* data, uint32_t len, gboolean copy, int* status, gboolean refonly, const char* name)
{
    TR("mono_image_open_from_data_with_name");
    (void)copy; (void)refonly;
    ensure_init();
    NrImage* img = image_by_file(name);
    check_hash(img, data, len);
    if (status) *status = img ? 0 : 3;  /* MONO_IMAGE_OK / IMAGE_INVALID */
    return img;
}
API NrImage* mono_image_open_from_data_full(char* data, uint32_t len, gboolean copy, int* status, gboolean refonly)
{
    TR("mono_image_open_from_data_full");
    (void)data; (void)len; (void)copy; (void)refonly;
    if (status) *status = 3;
    return 0;
}
API NrImage* mono_assembly_load_from_full(NrImage* img, const char* fname, int* status, gboolean refonly) { TR("mono_assembly_load_from_full"); (void)fname; (void)refonly; if (status) *status = img ? 0 : 3; return img; }
API NrImage* mono_assembly_load_from(NrImage* img, const char* fname, int* status) { TR("mono_assembly_load_from"); return mono_assembly_load_from_full(img, fname, status, 0); }
API NrImage* mono_assembly_open(const char* file, int* status) { TR("mono_assembly_open"); ensure_init(); NrImage* i = image_by_file(file); if (status) *status = i ? 0 : 3; return i; }
API NrImage* mono_domain_assembly_open(NrDomain* d, const char* name) { TR("mono_domain_assembly_open"); (void)d; return mono_assembly_open(name, 0); }
API NrImage* mono_assembly_get_image(NrImage* a) { TR("mono_assembly_get_image"); return a; }
API NrImage* mono_image_get_assembly(NrImage* i) { TR("mono_image_get_assembly"); return i; }
API void mono_assembly_close(NrImage* a) { TR("mono_assembly_close"); (void)a; }
API void mono_image_close(NrImage* i) { TR("mono_image_close"); (void)i; }
API const char* mono_image_get_name(NrImage* i) { TR("mono_image_get_name"); return i ? i->name : 0; }
API const char* mono_image_get_filename(NrImage* i) { TR("mono_image_get_filename"); return i ? i->file : 0; }
API int mono_image_get_table_rows(NrImage* i, int table) { TR("mono_image_get_table_rows"); return i && table == 2 ? i->n_typedefs : 0; }
API NrImage* mono_get_corlib(void) { TR("mono_get_corlib"); ensure_init(); return nr_k_object->image; }
API void mono_assembly_foreach(void (*fn)(void*, void*), void* user) { TR("mono_assembly_foreach"); for (int i = 0; i < nr_n_images; i++) fn(nr_images[i], user); }
/* MonoAssemblyName: only its first member, the name, is read or written here. */
API gboolean mono_assembly_name_parse(const char* name, const char** aname)
{
    TR("mono_assembly_name_parse");
    char* n = _strdup(name);
    char* c = strchr(n, ',');
    if (c) *c = 0;
    *aname = n;
    return 1;
}
API NrImage* mono_assembly_loaded(const char** aname)
{
    TR("mono_assembly_loaded");
    for (int i = 0; i < nr_n_images; i++) if (!_stricmp(nr_images[i]->name, *aname)) return nr_images[i];
    return 0;
}
API gboolean mono_assembly_fill_assembly_name(NrImage* img, const char** aname) { TR("mono_assembly_fill_assembly_name"); *aname = img->name; return 1; }
API char* mono_stringify_assembly_name(const char** aname) { TR("mono_stringify_assembly_name"); return _strdup(*aname); }
API void* mono_assembly_get_object(NrDomain* d, NrImage* a) { TR("mono_assembly_get_object"); (void)d; return nr_assembly_object(a); }

/* ---- classes ---- */

API NrClass* mono_class_get(NrImage* img, uint32_t token)
{
    TR("mono_class_get");
    TokEnt key = { img, token, 0 };
    TokEnt* e = (TokEnt*)bsearch(&key, tok_tab, (size_t)tok_n, sizeof(TokEnt), tok_cmp);
    return e ? e->k : 0;
}
API NrClass* mono_class_from_name(NrImage* img, const char* ns, const char* name)
{
    TR("mono_class_from_name");
    ensure_init();
    NrClass* k = nr_class_in_image(img, ns, name);
    if (trace_on > 0 || (!k && nr_debug())) nr_log("[mono]   %s: %s.%s -> %p\n", img ? img->name : "*", ns, name, (void*)k);
    return k;
}
API NrClass* mono_class_from_name_case(NrImage* img, const char* ns, const char* name)
{
    TR("mono_class_from_name_case");
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* k = nr_classes[i];
        if ((!img || k->image == img) && !_stricmp(k->name, name) && !_stricmp(k->ns, ns) && !(k->flags & (NR_ARRAY | NR_GENERIC_INST))) return k;
    }
    return 0;
}
API const char* mono_class_get_name(NrClass* k) { TR("mono_class_get_name"); return k->name; }
API const char* mono_class_get_namespace(NrClass* k) { TR("mono_class_get_namespace"); return k->ns; }
API NrClass* mono_class_get_parent(NrClass* k) { TR("mono_class_get_parent"); return (k->flags & NR_INTERFACE) ? 0 : k->parent; }
API NrImage* mono_class_get_image(NrClass* k) { TR("mono_class_get_image"); return k->image; }
API uint32_t mono_class_get_flags(NrClass* k) { TR("mono_class_get_flags"); return k->attrs; }
API int mono_class_get_rank(NrClass* k) { TR("mono_class_get_rank"); return k->rank; }
API NrClass* mono_class_get_element_class(NrClass* k) { TR("mono_class_get_element_class"); return k->elem ? k->elem : k; }
API NrClass* mono_class_get_nesting_type(NrClass* k) { TR("mono_class_get_nesting_type"); return k->nested_in; }
API NrType* mono_class_get_type(NrClass* k) { TR("mono_class_get_type"); return &k->byval_arg; }
API NrType* mono_class_get_byref_type(NrClass* k) { TR("mono_class_get_byref_type"); return &k->this_arg; }
API gboolean mono_class_is_valuetype(NrClass* k) { TR("mono_class_is_valuetype"); return (k->flags & NR_VALUETYPE) != 0; }
API gboolean mono_class_is_enum(NrClass* k) { TR("mono_class_is_enum"); return (k->flags & NR_ENUM) != 0; }
API gboolean mono_class_is_generic(NrClass* k) { TR("mono_class_is_generic"); if (trace_on > 0) nr_log("[mono]   %s\n", k->name); return !(k->flags & (NR_GENERIC_INST | NR_ARRAY)) && strchr(k->name, '`') != 0; }
API gboolean mono_class_is_inflated(NrClass* k) { TR("mono_class_is_inflated"); if (trace_on > 0) nr_log("[mono]   %s\n", k->name); return (k->flags & NR_GENERIC_INST) != 0; }
API gboolean mono_unity_class_is_abstract(NrClass* k) { TR("mono_unity_class_is_abstract"); return (k->flags & NR_ABSTRACT) != 0; }
API gboolean mono_unity_class_is_interface(NrClass* k) { TR("mono_unity_class_is_interface"); return (k->flags & NR_INTERFACE) != 0; }
API gboolean mono_class_is_subclass_of(NrClass* k, NrClass* p, gboolean ifaces)
{
    TR("mono_class_is_subclass_of");
    if (!ifaces && (p->flags & NR_INTERFACE)) return 0;
    return nr_assignable(p, k);
}
API NrType* mono_class_enum_basetype(NrClass* k) { TR("mono_class_enum_basetype"); return (k->flags & NR_ENUM) ? &k->elem->byval_arg : 0; }
API int32_t mono_class_instance_size(NrClass* k) { TR("mono_class_instance_size"); return k->instance_size; }
API int32_t mono_class_array_element_size(NrClass* k) { TR("mono_class_array_element_size"); return nr_value_size(k); }
API NrClass* mono_class_vtable(NrDomain* d, NrClass* k) { TR("mono_class_vtable"); (void)d; return k; }
API NrClass* mono_array_class_get(NrClass* e, uint32_t rank) { TR("mono_array_class_get"); return nr_array_class(e, (int)rank); }
API NrClass* mono_class_from_mono_type(NrType* t) { TR("mono_class_from_mono_type"); return t ? t->klass : 0; }

/* Iterators: *iter holds the next index + 1. */
API const NrFieldDesc* mono_class_get_fields(NrClass* k, void** iter)
{
    TR("mono_class_get_fields");
    uintptr_t i = (uintptr_t)*iter;
    if (i >= k->n_fields) return 0;
    *iter = (void*)(i + 1);
    return &k->fields[i];
}
API const NrMethodDesc* mono_class_get_methods(NrClass* k, void** iter)
{
    TR("mono_class_get_methods");
    uintptr_t i = (uintptr_t)*iter;
    if (i >= k->n_methods) return 0;
    *iter = (void*)(i + 1);
    return &k->methods[i];
}
API const NrProp* mono_class_get_properties(NrClass* k, void** iter)
{
    TR("mono_class_get_properties");
    int n;
    const NrProp* p = nr_props_of(k, &n);
    uintptr_t i = (uintptr_t)*iter;
    if ((int)i >= n) return 0;
    *iter = (void*)(i + 1);
    return &p[i];
}
API NrClass* mono_class_get_interfaces(NrClass* k, void** iter)
{
    TR("mono_class_get_interfaces");
    uintptr_t i = (uintptr_t)*iter;
    if (i >= k->n_ifaces) return 0;
    *iter = (void*)(i + 1);
    return k->ifaces[i].iface;
}
API const NrFieldDesc* mono_class_get_field_from_name(NrClass* k, const char* name) { TR("mono_class_get_field_from_name"); const NrFieldDesc* f = nr_field(k, name); if (trace_on > 0) nr_log("[mono]   %s::%s -> %p\n", k->name, name, (void*)f); return f; }
API const NrMethodDesc* mono_class_get_method_from_name(NrClass* k, const char* name, int n)
{
    TR("mono_class_get_method_from_name");
    for (int i = 0; i < k->n_methods; i++)
        if (!strcmp(k->methods[i].name, name) && (n < 0 || k->methods[i].n_params == n)) return &k->methods[i];
    return 0;
}
API const NrProp* mono_class_get_property_from_name(NrClass* k, const char* name)
{
    TR("mono_class_get_property_from_name");
    for (; k; k = k->parent)
    {
        int n;
        const NrProp* p = nr_props_of(k, &n);
        for (int i = 0; i < n; i++) if (!strcmp(p[i].name, name)) return &p[i];
    }
    return 0;
}

static NrClass* by_name(const char* n) { ensure_init(); return nr_class_by_name("System", n); }
API NrClass* mono_get_object_class(void) { TR("mono_get_object_class"); return by_name("Object"); }
API NrClass* mono_get_string_class(void) { TR("mono_get_string_class"); return by_name("String"); }
API NrClass* mono_get_int32_class(void) { TR("mono_get_int32_class"); return by_name("Int32"); }
API NrClass* mono_get_int16_class(void) { TR("mono_get_int16_class"); return by_name("Int16"); }
API NrClass* mono_get_int64_class(void) { TR("mono_get_int64_class"); return by_name("Int64"); }
API NrClass* mono_get_byte_class(void) { TR("mono_get_byte_class"); return by_name("Byte"); }
API NrClass* mono_get_char_class(void) { TR("mono_get_char_class"); return by_name("Char"); }
API NrClass* mono_get_boolean_class(void) { TR("mono_get_boolean_class"); return by_name("Boolean"); }
API NrClass* mono_get_single_class(void) { TR("mono_get_single_class"); return by_name("Single"); }
API NrClass* mono_get_double_class(void) { TR("mono_get_double_class"); return by_name("Double"); }
API NrClass* mono_get_array_class(void) { TR("mono_get_array_class"); return by_name("Array"); }
API NrClass* mono_get_enum_class(void) { TR("mono_get_enum_class"); return by_name("Enum"); }
API NrClass* mono_get_exception_class(void) { TR("mono_get_exception_class"); return by_name("Exception"); }

/* ---- types ---- */

API int mono_type_get_type(NrType* t) { TR("mono_type_get_type"); return t->type; }
/* mono returns data.klass: the element class for T[] (Unity relies on that for List<T>._items) */
API NrClass* mono_type_get_class(NrType* t) { TR("mono_type_get_class"); return t->type == 0x1d ? (NrClass*)t->data : t->klass; }
API void* mono_type_get_object(NrDomain* d, NrType* t) { TR("mono_type_get_object"); (void)d; return nr_type_object(t->klass); }
API char* mono_type_get_name(NrType* t)
{
    TR("mono_type_get_name");
    NrClass* k = t->klass;
    size_t n = strlen(k->ns) + strlen(k->name) + 3;
    char* s = (char*)malloc(n);
    snprintf(s, n, "%s%s%s%s", k->ns, *k->ns ? "." : "", k->name, NR_TYPE_BYREF(t) ? "&" : "");
    return s;
}

/* ---- fields ---- */

API const char* mono_field_get_name(const NrFieldDesc* f) { TR("mono_field_get_name"); return f->name; }
API NrType* mono_field_get_type(const NrFieldDesc* f) { TR("mono_field_get_type"); if (trace_on > 0) nr_log("[mono]   field %s.%s type %s.%s code %d flags 0x%x\n", f->parent->name, f->name, f->type->ns, f->type->name, f->type->byval_arg.type, f->flags); return f->byref ? &f->type->this_arg : &f->type->byval_arg; }
API int mono_field_get_offset(const NrFieldDesc* f) { TR("mono_field_get_offset"); return f->offset; }
API uint32_t mono_field_get_flags(const NrFieldDesc* f) { TR("mono_field_get_flags"); return f->flags; }
API void mono_field_get_value(void* obj, const NrFieldDesc* f, void* out) { TR("mono_field_get_value"); memcpy(out, (char*)obj + f->offset, (size_t)nr_value_size(f->type)); }
API void mono_field_set_value(void* obj, const NrFieldDesc* f, void* v)
{
    TR("mono_field_set_value");
    if (nr_is_ref(f->type)) *(void**)((char*)obj + f->offset) = v;
    else memcpy((char*)obj + f->offset, v, (size_t)nr_value_size(f->type));
}
API void mono_field_static_get_value(NrClass* vt, const NrFieldDesc* f, void* out)
{
    TR("mono_field_static_get_value");
    NrClass* k = f->parent ? f->parent : vt;
    if (k->cctor_state != 2) nr_run_cctor(k);
    if (f->flags & 0x40) { if (f->rva) memcpy(out, f->rva, (size_t)nr_value_size(f->type)); return; }
    memcpy(out, (char*)k->statics + f->offset, (size_t)nr_value_size(f->type));
}

/* ---- methods and signatures ---- */

API const char* mono_method_get_name(const NrMethodDesc* m) { TR("mono_method_get_name"); return m->name; }
API NrClass* mono_method_get_class(const NrMethodDesc* m) { TR("mono_method_get_class"); return m->klass; }
API const NrMethodDesc* mono_method_signature(const NrMethodDesc* m) { TR("mono_method_signature"); return m; }
API uint32_t mono_signature_get_param_count(const NrMethodDesc* s) { TR("mono_signature_get_param_count"); return s->n_params; }
API gboolean mono_signature_is_instance(const NrMethodDesc* s) { TR("mono_signature_is_instance"); return !(s->flags & 0x10); }
static NrType* param_type(const NrMethodDesc* s, int i)
{
    NrClass* k = s->params ? s->params[i] : 0;
    if (!k) k = nr_k_object;      /* a type the build never needed: read as object */
    return s->param_byref && s->param_byref[i] ? &k->this_arg : &k->byval_arg;
}
API NrType* mono_signature_get_params(const NrMethodDesc* s, void** iter)
{
    TR("mono_signature_get_params");
    uintptr_t i = (uintptr_t)*iter;
    if (i >= s->n_params) return 0;
    *iter = (void*)(i + 1);
    return param_type(s, (int)i);
}
API NrType* mono_signature_get_return_type(const NrMethodDesc* s) { TR("mono_signature_get_return_type"); return &(s->ret ? s->ret : nr_k_void)->byval_arg; }
API gboolean mono_metadata_signature_equal(const NrMethodDesc* a, const NrMethodDesc* b)
{
    TR("mono_metadata_signature_equal");
    if (a->n_params != b->n_params || a->ret != b->ret || ((a->flags ^ b->flags) & 0x10)) return 0;
    for (int i = 0; i < a->n_params; i++) if (param_type(a, i) != param_type(b, i)) return 0;
    return 1;
}
API void* mono_method_get_object(NrDomain* d, const NrMethodDesc* m, NrClass* refclass) { TR("mono_method_get_object"); (void)d; (void)refclass; return nr_method_object(m); }
API const NrMethodDesc* mono_property_get_get_method(const NrProp* p) { TR("mono_property_get_get_method"); return p->get; }

/* The override of m that obj's class actually runs. */
API const NrMethodDesc* mono_object_get_virtual_method(void* obj, const NrMethodDesc* m)
{
    TR("mono_object_get_virtual_method");
    if (!obj || m->slot < 0) return m;
    NrClass* c = ((NrObj*)obj)->klass;
    void* fn;
    if (m->klass->flags & NR_INTERFACE)
    {
        fn = 0;
        for (int i = 0; i < c->n_ifaces && !fn; i++)
            if (c->ifaces[i].iface == m->klass && c->ifaces[i].offset >= 0) fn = c->vtable[c->ifaces[i].offset + m->slot];
    }
    else fn = m->slot < c->n_vtable ? c->vtable[m->slot] : 0;
    if (!fn) return m;
    for (NrClass* k = c; k; k = k->parent)
        for (int i = 0; i < k->n_methods; i++)
            if (k->methods[i].fn == fn) return &k->methods[i];
    /* value-type overrides sit in the vtable as unboxing thunks: match by name */
    for (int i = 0; i < c->n_methods; i++)
        if (c->methods[i].slot == m->slot && !strcmp(c->methods[i].name, m->name)) return &c->methods[i];
    return m;
}

/* ---- objects ---- */

API void* mono_object_new(NrDomain* d, NrClass* k) { TR("mono_object_new"); (void)d; return nr_new(k); }
API void* mono_object_new_specific(NrClass* vt) { TR("mono_object_new_specific"); return nr_new(vt); }
API void* mono_object_new_alloc_specific(NrClass* vt) { TR("mono_object_new_alloc_specific"); return nr_new(vt); }
API NrClass* mono_object_get_class(void* o) { TR("mono_object_get_class"); return ((NrObj*)o)->klass; }
API void* mono_object_unbox(void* o) { TR("mono_object_unbox"); return (char*)o + sizeof(NrObj); }
API void* mono_value_box(NrDomain* d, NrClass* k, void* v) { TR("mono_value_box"); (void)d; return nr_box(k, v); }
API uint32_t mono_object_get_size(void* o)
{
    TR("mono_object_get_size");
    NrClass* k = ((NrObj*)o)->klass;
    if (k == nr_k_string) return 12 + 2 * ((uint32_t)((NrString*)o)->length + 1);
    if (k->flags & NR_ARRAY) return 16 + (uint32_t)k->elem_size * ((NrArray*)o)->max_length;
    return (uint32_t)k->instance_size;
}
API void mono_runtime_object_init(void* o)
{
    TR("mono_runtime_object_init");
    NrClass* k = ((NrObj*)o)->klass;
    for (int i = 0; i < k->n_methods; i++)
        if (!strcmp(k->methods[i].name, ".ctor") && k->methods[i].n_params == 0 && k->methods[i].fn)
        {
            void* ex = 0;
            nr_invoke(&k->methods[i], o, 0, &ex);
            if (ex && nr_debug()) nr_log("netrecomp: %s..ctor threw\n", k->name);
            return;
        }
}

API void* mono_runtime_invoke(const NrMethodDesc* m, void* obj, void** params, void** exc)
{
    TR("mono_runtime_invoke");
    if (!m) { if (exc) *exc = 0; return 0; }
    void* r = nr_invoke(m, obj, params, exc);
    if (trace_on > 0) nr_log("[mono]   %s::%s%s\n", m->klass->name, m->name, exc && *exc ? " threw" : "");
    return r;
}
API void* mono_runtime_delegate_invoke(void* d, void** params, void** exc)
{
    TR("mono_runtime_delegate_invoke");
    const NrMethodDesc* inv = nr_method(((NrObj*)d)->klass, "Invoke", -1);
    return nr_invoke(inv, d, params, exc);
}
API int mono_runtime_exec_main(const NrMethodDesc* m, void* args, void** exc)
{
    TR("mono_runtime_exec_main");
    void* a[1] = { args };
    void* r = nr_invoke(m, 0, m->n_params ? a : 0, exc);
    return r ? *(int32_t*)((char*)r + sizeof(NrObj)) : 0;
}

/* ---- strings ---- */

API void* mono_string_new_wrapper(const char* s) { TR("mono_string_new_wrapper"); return nr_str_utf8(s); }
API void* mono_string_new_len(NrDomain* d, const char* s, uint32_t len)
{
    TR("mono_string_new_len");
    (void)d;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, 0, 0);
    NrString* r = nr_str_alloc(n);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, (int)len, (LPWSTR)r->chars, n);
    return r;
}
API char* mono_string_to_utf8(void* s) { TR("mono_string_to_utf8"); return nr_utf8(s); }
API uint16_t* mono_string_to_utf16(void* s)
{
    TR("mono_string_to_utf16");
    NrString* str = (NrString*)s;
    if (!str) return 0;
    uint16_t* r = (uint16_t*)malloc(2 * ((size_t)str->length + 1));
    memcpy(r, str->chars, 2 * (size_t)str->length);
    r[str->length] = 0;
    return r;
}
API void* mono_string_from_utf16(const uint16_t* s) { TR("mono_string_from_utf16"); return nr_str_from_utf16z(s); }
API void g_free(void* p) { TR("g_free"); free(p); }

/* ---- arrays ---- */

API void* mono_array_new(NrDomain* d, NrClass* e, uintptr_t n) { TR("mono_array_new"); (void)d; return nr_newarr(nr_array_class(e, 1), (int32_t)n); }
API void* mono_array_new_full(NrDomain* d, NrClass* ak, uintptr_t* lens, intptr_t* lower)
{
    TR("mono_array_new_full");
    (void)d;
    int rank = ak->rank ? ak->rank : 1;
    if (rank == 1 && (!lower || !lower[0])) return nr_newarr(ak, (int32_t)lens[0]);
    int32_t l[32];
    for (int i = 0; i < rank && i < 32; i++) l[i] = (int32_t)lens[i];
    NrArray* a = (NrArray*)nr_new_mdarray(ak, rank, l);
    if (lower) for (int i = 0; i < rank; i++) a->bounds[i].lower_bound = (int32_t)lower[i];
    return a;
}

/* ---- exceptions ---- */

API void* mono_exception_from_name_msg(NrImage* img, const char* ns, const char* name, const char* msg)
{
    TR("mono_exception_from_name_msg");
    NrClass* k = nr_class_in_image(img, ns, name);
    if (!k) k = nr_class_by_name(ns, name);
    if (!k) k = nr_k_exception;
    return nr_exception(k, msg);
}
API void mono_raise_exception(void* e) { TR("mono_raise_exception"); nr_throw(e); }

/* ---- GC ---- */

API uint32_t mono_gchandle_new(void* o, gboolean pinned) { TR("mono_gchandle_new"); return nr_gchandle_new(o, pinned); }
API uint32_t mono_gchandle_new_weakref(void* o, gboolean track) { TR("mono_gchandle_new_weakref"); (void)track; return nr_gchandle_new_weak(o); }
API void* mono_gchandle_get_target(uint32_t h) { TR("mono_gchandle_get_target"); return nr_gchandle_target(h); }
API void mono_gchandle_free(uint32_t h)
{
    TR("mono_gchandle_free");
    if (trace_on > 0) { NrObj* o = (NrObj*)nr_gchandle_target(h); nr_log("[mono]   free %s %p\n", o ? o->klass->name : "-", (void*)o); }
    nr_gchandle_free(h);
}
API gboolean mono_gchandle_is_in_domain(uint32_t h, NrDomain* d) { TR("mono_gchandle_is_in_domain"); (void)h; (void)d; return 1; }
API void mono_gc_collect(int gen) { TR("mono_gc_collect"); (void)gen; GC_gcollect(); }
API int mono_gc_max_generation(void) { TR("mono_gc_max_generation"); return 0; }
API int64_t mono_gc_get_heap_size(void) { TR("mono_gc_get_heap_size"); return (int64_t)GC_get_heap_size(); }
API int64_t mono_gc_get_used_size(void) { TR("mono_gc_get_used_size"); return (int64_t)GC_get_heap_size() - (int64_t)GC_get_free_bytes(); }

/* Liveness: Resources.UnloadUnusedAssets (and scene loads) ask which
 * UnityEngine.Objects managed code reaches from each root Unity names and from
 * statics, and unload the rest. Results go to the callback at the end of each
 * from_root / from_statics call, because Unity attributes them to that root;
 * the visited set spans the whole calculation, so each object is reported once.
 * The walk is conservative: any word holding the start of a GC object whose
 * first word is a class record counts as a reference. Over-reporting only
 * keeps an asset loaded; under-reporting unloads one that is still in use. */
typedef void (*LivenessCb)(void** objs, int n, void* user);
typedef struct Liveness {
    NrClass* filter; LivenessCb cb; void* user;
    void** out; uint32_t n_out, max_out;
    void** seen; uint32_t seen_cap, n_seen;    /* open-addressing set */
    void** work; uint32_t n_work, work_cap;
} Liveness;

extern IMAGE_DOS_HEADER __ImageBase;
static NrClass* live_class(void* p)
{
    if (!p || ((uintptr_t)p & 3) || GC_base(p) != p) return 0;
    NrClass* k = *(NrClass**)p;
    if (!k || ((uintptr_t)k & 3)) return 0;
    char* img = (char*)&__ImageBase;
    size_t img_size = ((IMAGE_NT_HEADERS*)(img + __ImageBase.e_lfanew))->OptionalHeader.SizeOfImage;
    if (!((char*)k >= img && (char*)k < img + img_size) && !GC_base(k)) return 0;
    return k->self == k ? k : 0;
}
static int live_mark(Liveness* s, void* p)
{
    if (s->n_seen * 2 >= s->seen_cap)
    {
        uint32_t oc = s->seen_cap; void** old = s->seen;
        s->seen_cap = oc ? oc * 2 : 1 << 16;
        s->seen = (void**)calloc(s->seen_cap, sizeof(void*));
        s->n_seen = 0;
        for (uint32_t i = 0; i < oc; i++) if (old[i]) live_mark(s, old[i]);
        free(old);
    }
    uint32_t m = s->seen_cap - 1, h = (uint32_t)((uintptr_t)p >> 3) * 2654435761u & m;
    for (; s->seen[h]; h = (h + 1) & m) if (s->seen[h] == p) return 0;
    s->seen[h] = p; s->n_seen++;
    return 1;
}
static void live_flush(Liveness* s) { if (s->n_out) s->cb(s->out, (int)s->n_out, s->user); s->n_out = 0; }
static void live_push(Liveness* s, void* p)
{
    NrClass* k = live_class(p);
    if (!k || !live_mark(s, p)) return;
    if (!s->filter || nr_assignable(s->filter, k))
    {
        s->out[s->n_out++] = p;
        if (s->n_out == s->max_out) live_flush(s);
    }
    if (!(k->flags & NR_HAS_REFS)) return;
    if (s->n_work == s->work_cap) { s->work_cap = s->work_cap ? s->work_cap * 2 : 4096; s->work = (void**)realloc(s->work, s->work_cap * sizeof(void*)); }
    s->work[s->n_work++] = p;
}
static void live_scan(Liveness* s, void* const* from, size_t words)
{
    for (size_t i = 0; i < words; i++) live_push(s, from[i]);
    while (s->n_work)
    {
        void** o = (void**)s->work[--s->n_work];
        size_t n = GC_size(o) / sizeof(void*);
        for (size_t i = 2; i < n; i++) live_push(s, o[i]);
    }
}
API void* mono_unity_liveness_calculation_begin(NrClass* filter, uint32_t max, LivenessCb cb, void* user)
{
    TR("mono_unity_liveness_calculation_begin");
    Liveness* s = (Liveness*)calloc(1, sizeof *s);
    s->filter = filter; s->cb = cb; s->user = user;
    s->max_out = max && max < 64 ? max : 64;
    s->out = (void**)malloc(s->max_out * sizeof(void*));
    GC_disable();   /* nothing is freed while the walk holds raw pointers */
    return s;
}
API void mono_unity_liveness_calculation_end(Liveness* s)
{
    TR("mono_unity_liveness_calculation_end");
    live_flush(s);
    GC_enable();
    free(s->out); free(s->seen); free(s->work); free(s);
}
API void mono_unity_liveness_calculation_from_root(void* root, Liveness* s)
{
    TR("mono_unity_liveness_calculation_from_root");
    live_scan(s, &root, 1);
    live_flush(s);
}
API void mono_unity_liveness_calculation_from_statics(Liveness* s)
{
    TR("mono_unity_liveness_calculation_from_statics");
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* k = nr_classes[i];
        if (k && k->statics) live_scan(s, (void* const*)k->statics, k->statics_size / sizeof(void*));
    }
    live_flush(s);
}

/* ---- threads ---- */

API void* mono_thread_attach(NrDomain* d) { TR("mono_thread_attach"); (void)d; ensure_init(); nr_thread_attach(); return nr_current_thread_object(); }
API void* mono_thread_current(void) { TR("mono_thread_current"); return nr_current_thread_object(); }
API void mono_thread_detach(void* t) { TR("mono_thread_detach"); (void)t; }
API void mono_thread_exit(void) { TR("mono_thread_exit"); ExitThread(0); }
API void mono_thread_set_main(void* t) { TR("mono_thread_set_main"); (void)t; }
API void mono_thread_push_appdomain_ref(NrDomain* d) { TR("mono_thread_push_appdomain_ref"); (void)d; }
API void mono_thread_pop_appdomain_ref(void) { TR("mono_thread_pop_appdomain_ref");}
API void mono_thread_pool_cleanup(void) { TR("mono_thread_pool_cleanup");}
API void mono_thread_suspend_all_other_threads(void) { TR("mono_thread_suspend_all_other_threads");}
API void mono_threads_set_shutting_down(void) { TR("mono_threads_set_shutting_down");}
static int shutting_down;
API void mono_runtime_set_shutting_down(void) { TR("mono_runtime_set_shutting_down"); shutting_down = 1; }
API gboolean mono_runtime_is_shutting_down(void) { TR("mono_runtime_is_shutting_down"); return shutting_down; }
API void mono_runtime_cleanup(NrDomain* d) { TR("mono_runtime_cleanup"); (void)d; }

/* ---- stacks ----
 * ponytail: no managed frame records, so Unity's stack traces are empty. A
 * return-address -> descriptor table would give it real ones. */
API void mono_stack_walk(void* fn, void* user) { TR("mono_stack_walk"); (void)fn; (void)user; }
API const NrMethodDesc* mono_method_get_last_managed(void) { TR("mono_method_get_last_managed"); return 0; }
API char* mono_pmip(void* ip) { TR("mono_pmip"); (void)ip; return 0; }

/* ---- custom attributes ---- */

typedef struct NrAttrInfo { const NrAttr* a; int n; } NrAttrInfo;
static NrAttrInfo* attr_info(const NrAttr* a, int n)
{
    if (!n) return 0;
    NrAttrInfo* i = (NrAttrInfo*)malloc(sizeof *i);
    i->a = a; i->n = n;
    return i;
}
API NrAttrInfo* mono_custom_attrs_from_class(NrClass* k) { TR("mono_custom_attrs_from_class"); return attr_info(k->cattrs, k->n_cattrs); }
API NrAttrInfo* mono_custom_attrs_from_method(const NrMethodDesc* m) { TR("mono_custom_attrs_from_method"); return attr_info(m->cattrs, m->n_cattrs); }
API NrAttrInfo* mono_custom_attrs_from_field(NrClass* k, const NrFieldDesc* f) { TR("mono_custom_attrs_from_field"); (void)k; return attr_info(f->cattrs, f->n_cattrs); }
API gboolean mono_custom_attrs_has_attr(NrAttrInfo* i, NrClass* k)
{
    TR("mono_custom_attrs_has_attr");
    for (int j = 0; i && j < i->n; j++) if (nr_assignable(k, i->a[j].klass)) return 1;
    return 0;
}
API void* mono_custom_attrs_get_attr(NrAttrInfo* i, NrClass* k)
{
    TR("mono_custom_attrs_get_attr");
    for (int j = 0; i && j < i->n; j++) if (nr_assignable(k, i->a[j].klass)) return i->a[j].make();
    return 0;
}
API void mono_custom_attrs_free(NrAttrInfo* i) { TR("mono_custom_attrs_free"); free(i); }

/* called by the runtime for exceptions nothing caught on a thread */
void nr_report_unhandled(void* exc)
{
    if (unhandled_handler) unhandled_handler(exc);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { (void)h; (void)reason; (void)r; return TRUE; }
