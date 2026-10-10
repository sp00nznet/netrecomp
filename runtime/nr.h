/* nr.h: the contract between generated code (gen/*.c) and the netrecomp runtime.
 *
 * Object layout is mono's 32-bit embedding ABI, because Unity's native engine
 * reads object headers, strings and arrays directly. NrClass doubles as mono's
 * MonoClass and MonoVTable: an object's header points at it, and its first
 * word points back at itself (mono code reads vtable->klass).
 * See docs/runtime.md. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <excpt.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NrClass NrClass;
typedef struct NrAttr { NrClass* klass; void* (*make)(void); } NrAttr;
typedef struct NrObj { NrClass* klass; void* sync; } NrObj;
typedef struct NrString { NrObj o; int32_t length; uint16_t chars[1]; } NrString;
typedef struct NrBounds { uint32_t length; int32_t lower_bound; } NrBounds;
typedef struct NrArray { NrObj o; NrBounds* bounds; uint32_t max_length; double vector[1]; } NrArray;
typedef struct NrDelegate {
    NrObj o;
    void* method_ptr; void* invoke_impl; void* target; void* method;
    void* delegate_trampoline; void* method_code; void* method_info; void* original_method_info; void* data;
    void* prev; void* kpm_next;
} NrDelegate;

/* MonoType, laid out as mono's (Unity reads it directly): data is the class
 * for classes and value types, the element class for T[], a MonoArrayType for
 * T[,], a MonoGenericClass for generic instances, and the element MonoType for
 * pointers. bits: num_mods:6, byref:1 (0x40), pinned:1. klass is ours. */
typedef struct NrType { void* data; uint16_t attrs; uint8_t type; uint8_t bits; NrClass* klass; } NrType;
#define NR_TYPE_BYREF(t) (((t)->bits & 0x40) != 0)
/* what MonoType.data points at for T[,] and generic instances (mono 2.x layouts) */
typedef struct NrMonoArrayType { NrClass* eklass; uint8_t rank, numsizes, numlobounds; int* sizes; int* lobounds; } NrMonoArrayType;
typedef struct NrMonoGenericClass { NrClass* container_class; void* class_inst; void* method_inst; uint32_t flags; NrClass* cached_class; } NrMonoGenericClass;
typedef struct NrIface { NrClass* iface; int32_t offset; } NrIface;
/* One per assembly. sha256 is the file the code was generated from: the mono
 * host checks it against what the game actually loads. */
typedef struct NrImage {
    const char* name; const char* file; void* assembly_obj; void* reserved;
    int32_t n_typedefs; const char* fullname; const char* sha256;
} NrImage;

typedef struct NrFieldDesc {
    const char* name; NrClass* type; NrClass* parent;
    int32_t offset;           /* from object start (instance) or statics block (static) */
    uint16_t flags;           /* FieldAttributes */
    uint16_t byref;
    uint32_t token;
    const void* rva; int32_t rva_size;
    const NrAttr* cattrs; uint16_t n_cattrs;
} NrFieldDesc;

/* mono_runtime_invoke shape: args[i] is the object for reference types and
 * byrefs, else a pointer to the value; the raw return lands in *ret. */
typedef void (*NrInvoker)(void* fn, void* obj, void** args, void* ret);
typedef struct NrMethodDesc {
    NrClass* klass; const char* name; void* fn; NrInvoker invoke;
    uint16_t flags; uint16_t iflags;  /* MethodAttributes, MethodImplAttributes */
    uint16_t n_params; int16_t slot;
    NrClass* ret; NrClass* const* params; const uint8_t* param_byref;
    uint32_t token;
    const NrAttr* cattrs; uint16_t n_cattrs;
} NrMethodDesc;

enum {
    NR_VALUETYPE = 1, NR_ENUM = 2, NR_INTERFACE = 4, NR_ABSTRACT = 8, NR_ARRAY = 16, NR_HAS_REFS = 32,
    NR_SEALED = 64, NR_GENERIC_INST = 128, NR_DELEGATE = 256, NR_NULLABLE = 512, NR_HAS_FINALIZER = 1024,
    NR_STRING = 2048, NR_CONSTRUCTED = 4096, NR_POINTER = 8192, NR_BYREF_ONLY = 16384,
};

struct NrClass {
    NrClass* self;
    const char* name; const char* ns;
    NrClass* parent; NrClass* elem; NrClass* cast_class; NrClass* nested_in; NrClass* gdef;
    NrClass* const* gargs; uint16_t n_gargs;
    uint16_t rank; uint16_t depth;
    uint32_t flags; uint32_t attrs;
    int32_t instance_size; int32_t value_size; int32_t elem_size;
    NrClass* const* supers;
    const NrIface* ifaces; uint16_t n_ifaces;
    uint16_t n_vtable; void* const* vtable;
    void (*cctor)(void); volatile int32_t cctor_state; volatile uint32_t cctor_thread;
    void* statics; uint32_t statics_size;
    const NrFieldDesc* fields; uint16_t n_fields;
    uint16_t n_methods; const NrMethodDesc* methods;
    NrImage* image; uint32_t token;
    NrType byval_arg, this_arg;
    void* type_obj;           /* cached System.MonoType */
    NrClass* szarray;         /* T[] when generated */
    void* rt;                 /* runtime-private */
    const NrAttr* cattrs; uint16_t n_cattrs;
    const struct NrRevPool* rev_pool;   /* delegates handed to native code */
};

typedef struct NrRevPool { uint32_t* handles; void* const* fns; int32_t n; } NrRevPool;
void* nr_reverse_enter(uint32_t handle);

typedef struct NrStrLit { const uint16_t* chars; int32_t len; } NrStrLit;
typedef struct NrGvmEntry { NrClass* klass; void* fn; } NrGvmEntry;
typedef struct NrGvm { const NrGvmEntry* entries; } NrGvm;

/* Generated tables */
extern void* nr_strtab[];
extern const NrStrLit nr_strlits[];
extern NrClass* const nr_classes[];
extern const int32_t nr_n_classes;
extern NrImage* const nr_images[];
extern const int32_t nr_n_images;

/* Runtime entry points */
void* nr_new(NrClass* k);
void* nr_newarr(NrClass* arrk, int32_t n);
void* nr_new_mdarray(NrClass* arrk, int rank, const int32_t* lens);
void* nr_mdarray_addr(void* a, int rank, const int32_t* idx);
void* nr_box(NrClass* k, const void* data);
void* nr_box_nullable(NrClass* inner, const void* nullable, size_t has_off, size_t val_off);
void  nr_unbox_nullable(void* o, NrClass* inner, void* out, size_t size, size_t has_off, size_t val_off);
void* nr_unbox(void* o, NrClass* k);
void* nr_castclass(void* o, NrClass* k);
void* nr_isinst(void* o, NrClass* k);
void* nr_iface(void* o, NrClass* iface, int k);
void* nr_gvm(void* o, const NrGvm* g);
void  nr_stelem_ref(void* a, int32_t i, void* v);
void* nr_strlit(int i);
void  nr_run_cctor(NrClass* k);
void  nr_delegate_init(void* d, void* target, void* fn, const NrMethodDesc* m);
void* nr_delegate_to_fnptr(void* d);
NrType* nr_class_type(NrClass* k);
void* nr_icall(const char* full, const char* name);
void* nr_pinvoke(const char* lib, const char* entry, const char* alt);
int64_t nr_native_call(void* fn, const uint32_t* args, int32_t n);
double nr_native_call_fp(void* fn, const uint32_t* args, int32_t n);
char* nr_str_to_utf8(void* s);
const uint16_t* nr_str_chars(void* s);
void* nr_str_from_utf8(const char* s);
void* nr_str_from_utf16z(const uint16_t* s);
void  nr_free(void* p);
void* nr_alloc_root(size_t n);                       /* scanned, never collected; nr_free_root */
void  nr_async_start(void (*fn)(void*), void* arg);  /* runs fn(arg) on a new managed thread, frees arg */
void  nr_async_callback(void* cb);

__declspec(noreturn) void nr_throw(void* exc);
__declspec(noreturn) void nr_throw_null(void);
__declspec(noreturn) void nr_throw_range(void);
__declspec(noreturn) void nr_throw_overflow(void);
__declspec(noreturn) void nr_throw_divzero(void);
__declspec(noreturn) void nr_throw_notsupported(const char* what);
__declspec(noreturn) void nr_throw_arith(void);
int nr_catch(struct _EXCEPTION_POINTERS* ep, NrClass* k, void** ex);
int nr_catch_any(struct _EXCEPTION_POINTERS* ep, NrClass* const* ks, int n, void** ex, int32_t* which);

#define NR_STR(i) (nr_strtab[i] ? nr_strtab[i] : nr_strlit(i))
#define NR_CCTOR(k) do { if ((k).cctor_state != 2) nr_run_cctor(&(k)); } while (0)
#define NR_VT(o) (((NrObj*)(o))->klass->vtable)
#define NR_NULLCHK(o) do { if (!(o)) nr_throw_null(); } while (0)
#define NR_ADATA(a) ((void*)((NrArray*)(a))->vector)
#define NR_AIDX(a, i) nr_aidx((NrArray*)(a), (int32_t)(i))

static __inline int32_t nr_aidx(NrArray* a, int32_t i) { if ((uint32_t)i >= a->max_length) nr_throw_range(); return i; }
static __inline void* nr_zalloca(void* p, uint32_t n) { memset(p, 0, n); return p; }
static __inline float nr_f32(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static __inline double nr_f64(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }

/* Integer division: CLI throws where C has undefined behaviour. */
static __inline int32_t nr_div_i4(int32_t a, int32_t b) { if (!b) nr_throw_divzero(); if (b == -1 && a == INT32_MIN) nr_throw_arith(); return a / b; }
static __inline int32_t nr_rem_i4(int32_t a, int32_t b) { if (!b) nr_throw_divzero(); if (b == -1) return 0; return a % b; }
static __inline uint32_t nr_divu_i4(uint32_t a, uint32_t b) { if (!b) nr_throw_divzero(); return a / b; }
static __inline uint32_t nr_remu_i4(uint32_t a, uint32_t b) { if (!b) nr_throw_divzero(); return a % b; }
static __inline int64_t nr_div_i8(int64_t a, int64_t b) { if (!b) nr_throw_divzero(); if (b == -1 && a == INT64_MIN) nr_throw_arith(); return a / b; }
static __inline int64_t nr_rem_i8(int64_t a, int64_t b) { if (!b) nr_throw_divzero(); if (b == -1) return 0; return a % b; }
static __inline uint64_t nr_divu_i8(uint64_t a, uint64_t b) { if (!b) nr_throw_divzero(); return a / b; }
static __inline uint64_t nr_remu_i8(uint64_t a, uint64_t b) { if (!b) nr_throw_divzero(); return a % b; }

/* Float -> int: x86 returns the "integer indefinite" value out of range, and
 * so does mono's x87 code. C leaves it undefined, so do it by hand. */
static __inline int32_t nr_d2i4(double d) { return (d != d || d >= 2147483648.0 || d <= -2147483649.0) ? INT32_MIN : (int32_t)d; }
static __inline int64_t nr_d2i8(double d) { return (d != d || d >= 9223372036854775808.0 || d < -9223372036854775808.0) ? INT64_MIN : (int64_t)d; }
static __inline uint64_t nr_d2u8(double d) { if (d != d || d <= -1.0 || d >= 18446744073709551616.0) return 0; return d >= 9223372036854775808.0 ? (uint64_t)(d - 9223372036854775808.0) + 0x8000000000000000ull : (uint64_t)d; }
static __inline float nr_ckfinite_f(float f) { if (f != f || f - f != 0) nr_throw_arith(); return f; }
static __inline double nr_ckfinite(double d) { if (d != d || d - d != 0) nr_throw_arith(); return d; }

/* Overflow-checked arithmetic */
static __inline int32_t nr_add_ovf_i4(int32_t a, int32_t b) { int64_t r = (int64_t)a + b; if (r != (int32_t)r) nr_throw_overflow(); return (int32_t)r; }
static __inline int32_t nr_sub_ovf_i4(int32_t a, int32_t b) { int64_t r = (int64_t)a - b; if (r != (int32_t)r) nr_throw_overflow(); return (int32_t)r; }
static __inline int32_t nr_mul_ovf_i4(int32_t a, int32_t b) { int64_t r = (int64_t)a * b; if (r != (int32_t)r) nr_throw_overflow(); return (int32_t)r; }
static __inline int32_t nr_addu_ovf_i4(uint32_t a, uint32_t b) { uint64_t r = (uint64_t)a + b; if (r >> 32) nr_throw_overflow(); return (int32_t)r; }
static __inline int32_t nr_subu_ovf_i4(uint32_t a, uint32_t b) { if (b > a) nr_throw_overflow(); return (int32_t)(a - b); }
static __inline int32_t nr_mulu_ovf_i4(uint32_t a, uint32_t b) { uint64_t r = (uint64_t)a * b; if (r >> 32) nr_throw_overflow(); return (int32_t)r; }
static __inline int64_t nr_add_ovf_i8(int64_t a, int64_t b) { int64_t r = (int64_t)((uint64_t)a + (uint64_t)b); if (((a ^ r) & (b ^ r)) < 0) nr_throw_overflow(); return r; }
static __inline int64_t nr_sub_ovf_i8(int64_t a, int64_t b) { int64_t r = (int64_t)((uint64_t)a - (uint64_t)b); if (((a ^ b) & (a ^ r)) < 0) nr_throw_overflow(); return r; }
static __inline int64_t nr_mul_ovf_i8(int64_t a, int64_t b)
{
    if (a == 0 || b == 0) return 0;
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) nr_throw_overflow();
    int64_t r = (int64_t)((uint64_t)a * (uint64_t)b);
    if (r / b != a) nr_throw_overflow();
    return r;
}
static __inline int64_t nr_addu_ovf_i8(uint64_t a, uint64_t b) { if (a + b < a) nr_throw_overflow(); return (int64_t)(a + b); }
static __inline int64_t nr_subu_ovf_i8(uint64_t a, uint64_t b) { if (b > a) nr_throw_overflow(); return (int64_t)(a - b); }
static __inline int64_t nr_mulu_ovf_i8(uint64_t a, uint64_t b) { if (a && b > UINT64_MAX / a) nr_throw_overflow(); return (int64_t)(a * b); }

/* conv.ovf.<to>[.un] from <i4|i8|r8> */
#define NR_OVF(to, ctype, lo, hi) \
    static __inline ctype nr_ovf_##to##_i4(int32_t v) { if ((int64_t)v < (int64_t)(lo) || (int64_t)v > (int64_t)(hi)) nr_throw_overflow(); return (ctype)v; } \
    static __inline ctype nr_ovf_##to##_i8(int64_t v) { if (v < (int64_t)(lo) || v > (int64_t)(hi)) nr_throw_overflow(); return (ctype)v; } \
    static __inline ctype nr_ovf_##to##_r8(double v) { if (!(v > (double)(lo) - 1.0 && v < (double)(hi) + 1.0)) nr_throw_overflow(); return (ctype)v; } \
    static __inline ctype nr_ovf_##to##_un_i4(int32_t v) { if ((uint64_t)(uint32_t)v > (uint64_t)(hi)) nr_throw_overflow(); return (ctype)(uint32_t)v; } \
    static __inline ctype nr_ovf_##to##_un_i8(int64_t v) { if ((uint64_t)v > (uint64_t)(hi)) nr_throw_overflow(); return (ctype)(uint64_t)v; } \
    static __inline ctype nr_ovf_##to##_un_r8(double v) { if (!(v > -1.0 && v < (double)(hi) + 1.0)) nr_throw_overflow(); return (ctype)v; }
NR_OVF(i1, int32_t, -128, 127)
NR_OVF(u1, int32_t, 0, 255)
NR_OVF(i2, int32_t, -32768, 32767)
NR_OVF(u2, int32_t, 0, 65535)
NR_OVF(i4, int32_t, INT32_MIN, INT32_MAX)
NR_OVF(u4, int32_t, 0, UINT32_MAX)
NR_OVF(i, int32_t, INT32_MIN, INT32_MAX)
NR_OVF(u, int32_t, 0, UINT32_MAX)
NR_OVF(i8, int64_t, INT64_MIN, INT64_MAX)
#undef NR_OVF
static __inline int64_t nr_ovf_u8_i4(int32_t v) { if (v < 0) nr_throw_overflow(); return v; }
static __inline int64_t nr_ovf_u8_i8(int64_t v) { if (v < 0) nr_throw_overflow(); return v; }
static __inline int64_t nr_ovf_u8_r8(double v) { if (!(v > -1.0 && v < 18446744073709551616.0)) nr_throw_overflow(); return (int64_t)nr_d2u8(v); }
static __inline int64_t nr_ovf_u8_un_i4(int32_t v) { return (int64_t)(uint32_t)v; }
static __inline int64_t nr_ovf_u8_un_i8(int64_t v) { return v; }
static __inline int64_t nr_ovf_u8_un_r8(double v) { return nr_ovf_u8_r8(v); }

#ifdef __cplusplus
}
#endif
