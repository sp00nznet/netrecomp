/* nrrt.h: runtime-internal API shared by runtime/*.c. Generated code only
 * sees nr.h. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "nr.h"
#ifndef GC_THREADS
#define GC_THREADS
#endif
#ifndef GC_NOT_DLL
#define GC_NOT_DLL
#endif
#include "gc.h"

#define NR_EXC_CODE 0xE04E5258u   /* 'NRX': a managed throw; ExceptionInformation[0] is the object */

typedef struct NrIcall { const char* name; void* fn; } NrIcall;

/* classes the runtime needs by name, resolved in nr_init */
extern NrClass *nr_k_object, *nr_k_string, *nr_k_array, *nr_k_monotype, *nr_k_void, *nr_k_int32,
    *nr_k_bool, *nr_k_char, *nr_k_byte, *nr_k_int64, *nr_k_double, *nr_k_single, *nr_k_intptr,
    *nr_k_exception, *nr_k_valuetype, *nr_k_enum, *nr_k_delegate, *nr_k_thread, *nr_k_objarr;

void nr_init(void);
void nr_register_icalls(const NrIcall* t, int n);
void nr_add_icall(const char* name, const void* fn);

NrClass* nr_class_by_name(const char* ns, const char* name);
NrClass* nr_class_in_image(const NrImage* img, const char* ns, const char* name);
const NrFieldDesc* nr_field(NrClass* k, const char* name);
const NrMethodDesc* nr_method(NrClass* k, const char* name, int nparams);
NrClass* nr_array_class(NrClass* elem, int rank);
int nr_assignable(NrClass* to, NrClass* from);
NrClass* nr_class_of(void* o);
int32_t nr_value_size(NrClass* k);   /* bytes a value of k takes in a field or array slot */
int nr_is_ref(NrClass* k);

NrString* nr_str_alloc(int32_t len);
NrString* nr_str_utf16(const uint16_t* s, int32_t len);
NrString* nr_str_utf8(const char* s);
char* nr_utf8(void* s);                   /* malloc'd; nr_free */
void* nr_new_string_array(int n);

void* nr_exception(NrClass* k, const char* msg);
__declspec(noreturn) void nr_throw_new(const char* ns, const char* name, const char* msg);
void* nr_current_exception_object(struct _EXCEPTION_POINTERS* ep);

void* nr_type_object(NrClass* k);
NrClass* nr_type_class(void* typeobj);
int nr_type_byref(void* typeobj);

/* mono_runtime_invoke: returns boxed result (or the object); *exc gets a thrown exception */
void* nr_invoke(const NrMethodDesc* m, void* obj, void** args, void** exc);
void* nr_box_raw(NrClass* k, const void* raw);
void* nr_virtual_desc(const NrMethodDesc* m, void* obj, const NrMethodDesc** out);

/* threads */
void nr_thread_attach(void);
void* nr_current_thread_object(void);

/* gc handles */
uint32_t nr_gchandle_new(void* o, int pinned);
uint32_t nr_gchandle_new_weak(void* o);
void* nr_gchandle_target(uint32_t h);
void nr_gchandle_free(uint32_t h);

/* hooks the Unity host sets: plugin lookup for P/Invoke, unhandled exceptions */
extern void* (*nr_pinvoke_hook)(const char* lib, const char* entry);
extern void (*nr_unhandled_hook)(void* exc);

typedef struct NrProp { NrClass* klass; const char* name; const NrMethodDesc *get, *set; } NrProp;
const NrProp* nr_props_of(NrClass* k, int* n);
void* nr_method_object(const NrMethodDesc* m);
void* nr_assembly_object(NrImage* img);
extern int nr_log_missing;
void nr_log(const char* fmt, ...);
