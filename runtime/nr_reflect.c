/* nr_reflect.c: reflection internal calls answered from the generated class
 * records (types, enums, fields, methods, constructors, properties,
 * assemblies). Only what was compiled exists: a method nothing reached has a
 * descriptor but no code, and invoking it throws MissingMethodException.
 * See docs/runtime.md. */
#include "nrrt.h"

#define OBJ(o) ((NrObj*)(o))
#define STR(o) ((NrString*)(o))
#define ARR(o) ((NrArray*)(o))
#define K(t) nr_type_class(t)

static NrClass *k_monomethod, *k_monocmethod, *k_monofield, *k_monoproperty, *k_assembly, *k_paraminfo, *k_module;

static int32_t off(NrClass* k, const char* n)
{
    const NrFieldDesc* f = nr_field(k, n);
    if (!f) { nr_log("netrecomp: field %s::%s missing\n", k->name, n); return 0; }
    return f->offset;
}
#define SETF(o, T, k, n, v) (*(T*)((char*)(o) + off(k, n)) = (v))
#define GETF(o, T, k, n) (*(T*)((char*)(o) + off(k, n)))

static void* new_array(NrClass* elem, int n) { return nr_newarr(nr_array_class(elem, 1), n); }
static void** adata(void* a) { return (void**)ARR(a)->vector; }

/* ---- member objects, one per descriptor ---- */

typedef struct Cache { const void* key; void* obj; struct Cache* next; } Cache;
static Cache* cache_tab[4096];
static CRITICAL_SECTION cache_lock;
static void* cache_get(const void* key)
{
    EnterCriticalSection(&cache_lock);
    for (Cache* c = cache_tab[((uintptr_t)key >> 3) & 4095]; c; c = c->next)
        if (c->key == key) { LeaveCriticalSection(&cache_lock); return c->obj; }
    LeaveCriticalSection(&cache_lock);
    return 0;
}
static void* cache_put(const void* key, void* obj)
{
    EnterCriticalSection(&cache_lock);
    uint32_t h = ((uintptr_t)key >> 3) & 4095;
    for (Cache* c = cache_tab[h]; c; c = c->next)
        if (c->key == key) { LeaveCriticalSection(&cache_lock); return c->obj; }
    Cache* c = (Cache*)GC_MALLOC_UNCOLLECTABLE(sizeof *c);
    c->key = key; c->obj = obj; c->next = cache_tab[h]; cache_tab[h] = c;
    LeaveCriticalSection(&cache_lock);
    return obj;
}

static int is_ctor(const NrMethodDesc* m) { return m->name[0] == '.' && (!strcmp(m->name, ".ctor") || !strcmp(m->name, ".cctor")); }

void* nr_method_object(const NrMethodDesc* m)
{
    if (!m) return 0;
    void* o = cache_get(m);
    if (o) return o;
    NrClass* k = is_ctor(m) ? k_monocmethod : k_monomethod;
    o = nr_new(k);
    SETF(o, const NrMethodDesc*, k, "mhandle", m);
    return cache_put(m, o);
}

static void* field_object(const NrFieldDesc* f)
{
    void* o = cache_get(f);
    if (o) return o;
    o = nr_new(k_monofield);
    SETF(o, NrClass*, k_monofield, "klass", f->parent);
    SETF(o, const NrFieldDesc*, k_monofield, "fhandle", f);
    SETF(o, void*, k_monofield, "name", nr_str_utf8(f->name));
    SETF(o, void*, k_monofield, "type", nr_type_object(f->type));
    SETF(o, int32_t, k_monofield, "attrs", f->flags);
    return cache_put(f, o);
}

/* Properties are synthesized from get_X / set_X special-name methods. */
typedef struct PropList { NrClass* k; NrProp* p; int n; struct PropList* next; } PropList;
static PropList* props;
const NrProp* nr_props_of(NrClass* k, int* n)
{
    EnterCriticalSection(&cache_lock);
    for (PropList* l = props; l; l = l->next) if (l->k == k) { *n = l->n; LeaveCriticalSection(&cache_lock); return l->p; }
    NrProp* p = (NrProp*)calloc(k->n_methods + 1, sizeof *p);
    int c = 0;
    for (int i = 0; i < k->n_methods; i++)
    {
        const NrMethodDesc* m = &k->methods[i];
        if (!(m->flags & 0x800) || strlen(m->name) < 5 || m->name[3] != '_') continue;
        int get = !strncmp(m->name, "get_", 4), set = !strncmp(m->name, "set_", 4);
        if (!get && !set) continue;
        const char* nm = m->name + 4;
        int j = 0;
        /* indexers overload by parameters (IDataRecord.this[int] / this[string]):
         * pair a getter and setter only when the setter's index parameters match */
        for (; j < c; j++)
        {
            if (strcmp(p[j].name, nm) || (get ? p[j].get : p[j].set)) continue;
            const NrMethodDesc* g = get ? m : p[j].get;
            const NrMethodDesc* st = get ? p[j].set : m;
            if (!g || !st) break;
            int ok = st->n_params == g->n_params + 1;
            for (int q = 0; ok && q < g->n_params; q++) ok = g->params[q] == st->params[q];
            if (ok) break;
        }
        if (j == c) { p[c].klass = k; p[c].name = nm; c++; }
        if (get) p[j].get = m; else p[j].set = m;
    }
    PropList* l = (PropList*)malloc(sizeof *l);
    l->k = k; l->p = p; l->n = c; l->next = props; props = l;
    *n = c;
    LeaveCriticalSection(&cache_lock);
    return p;
}
static void* prop_object(const NrProp* p)
{
    void* o = cache_get(p);
    if (o) return o;
    o = nr_new(k_monoproperty);
    SETF(o, NrClass*, k_monoproperty, "klass", p->klass);
    SETF(o, const NrProp*, k_monoproperty, "prop", p);
    return cache_put(p, o);
}

/* ---- System.Type / MonoType ---- */

static void* Type_internal_from_handle(intptr_t h) { return h ? nr_type_object(((NrType*)h)->klass) : 0; }
static uint8_t Type_EqualsInternal(void* self, void* t) { return t && K(self) == K(t); }
static uint8_t Type_IsArrayImpl(void* t) { return (K(t)->flags & NR_ARRAY) != 0; }
static uint8_t Type_IsInstanceOfType(void* self, void* o) { return nr_isinst(o, K(self)) != 0; }
static uint8_t Type_type_is_assignable_from(void* a, void* b) { return nr_assignable(K(a), K(b)); }
static uint8_t Type_type_is_subtype_of(void* a, void* b, uint8_t ifaces)
{
    NrClass *ka = K(a), *kb = K(b);
    if (!ifaces && (kb->flags & NR_INTERFACE)) return 0;
    return ka != kb && nr_assignable(kb, ka);
}
static uint8_t Type_get_IsGenericType(void* self) { NrClass* k = K(self); return (k->flags & NR_GENERIC_INST) || (k->n_gargs == 0 && strchr(k->name, '`') != 0 && !(k->flags & NR_ARRAY)); }
static uint8_t Type_get_IsGenericTypeDefinition(void* self) { NrClass* k = K(self); return !(k->flags & NR_GENERIC_INST) && strchr(k->name, '`') != 0 && !(k->flags & NR_ARRAY); }
static void* Type_GetGenericTypeDefinition_impl(void* self)
{
    NrClass* k = K(self);
    if (!(k->flags & NR_GENERIC_INST)) return Type_get_IsGenericTypeDefinition(self) ? self : 0;
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* c = nr_classes[i];
        if (c->image == k->image && !(c->flags & NR_GENERIC_INST) && !strcmp(c->name, k->name) && !strcmp(c->ns, k->ns))
            return nr_type_object(c);
    }
    nr_log("netrecomp: no generic definition record for %s.%s\n", k->ns, k->name);
    return 0;
}
static void* Type_MakeGenericType(void* gt, void* types)
{
    NrClass* d = K(gt);
    int n = (int)ARR(types)->max_length;
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* c = nr_classes[i];
        if (!(c->flags & NR_GENERIC_INST) || c->n_gargs != n || c->image != d->image || strcmp(c->name, d->name) || strcmp(c->ns, d->ns)) continue;
        int ok = 1;
        for (int j = 0; j < n && ok; j++) ok = c->gargs[j] == K(adata(types)[j]);
        if (ok) return nr_type_object(c);
    }
    char msg[600];
    int at = _snprintf(msg, sizeof msg, "%s.%s<", d->ns, d->name);
    for (int j = 0; j < n && at > 0 && at < 500; j++) at += _snprintf(msg + at, sizeof msg - at, "%s%s.%s", j ? "," : "", K(adata(types)[j])->ns, K(adata(types)[j])->name);
    if (at > 0 && at < 560) _snprintf(msg + at, sizeof msg - at, "> was not compiled");
    msg[sizeof msg - 1] = 0;
    nr_throw_notsupported(msg);
}
static int32_t Type_GetTypeCodeInternal(void* t)
{
    NrClass* k = K(t);
    if (k->flags & NR_ENUM) k = k->elem;
    switch (k->byval_arg.type)
    {
    case 2: return 3; case 3: return 4; case 4: return 5; case 5: return 6; case 6: return 7; case 7: return 8;
    case 8: return 9; case 9: return 10; case 0xa: return 11; case 0xb: return 12; case 0xc: return 13; case 0xd: return 14;
    case 0xe: return 18;
    }
    if (!strcmp(k->ns, "System"))
    {
        if (!strcmp(k->name, "Decimal")) return 15;
        if (!strcmp(k->name, "DateTime")) return 16;
        if (!strcmp(k->name, "DBNull")) return 2;
    }
    return 1;  /* Object */
}
static void* Type_make_array_type(void* self, int32_t rank) { return nr_type_object(nr_array_class(K(self), rank ? rank : 1)); }
static void* Type_make_byref_type(void* self) { (void)self; nr_throw_notsupported("Type.MakeByRefType"); }
static void* Type_MakePointerType(void* self) { (void)self; nr_throw_notsupported("Type.MakePointerType"); }
static void Type_GetPacking(void* self, int32_t* packing, int32_t* size) { (void)self; *packing = 0; *size = 0; }

static void* MonoType_get_Name(void* self) { return nr_str_utf8(K(self)->name); }
static void* MonoType_get_Namespace(void* self)
{
    NrClass* k = K(self);
    while (k->nested_in) k = k->nested_in;
    return nr_str_utf8(k->ns);
}
static void full_name(NrClass* k, char* buf, size_t n, int args)
{
    if (k->nested_in) { full_name(k->nested_in, buf, n, 0); strncat(buf, "+", n - strlen(buf) - 1); }
    else if (*k->ns) { strncat(buf, k->ns, n - strlen(buf) - 1); strncat(buf, ".", n - strlen(buf) - 1); }
    strncat(buf, k->name, n - strlen(buf) - 1);
    if (args && k->n_gargs)
    {
        strncat(buf, "[", n - strlen(buf) - 1);
        for (int i = 0; i < k->n_gargs; i++)
        {
            if (i) strncat(buf, ",", n - strlen(buf) - 1);
            full_name(k->gargs[i], buf, n, 1);
        }
        strncat(buf, "]", n - strlen(buf) - 1);
    }
}
static void* MonoType_getFullName(void* self, uint8_t full, uint8_t aq)
{
    char buf[2048] = { 0 };
    NrClass* k = K(self);
    if (k->flags & NR_ARRAY) { full_name(k->elem, buf, sizeof buf, full); strncat(buf, k->name + strlen(k->elem->name), sizeof buf - strlen(buf) - 1); }
    else full_name(k, buf, sizeof buf, full);
    if (aq && k->image) { strncat(buf, ", ", sizeof buf - strlen(buf) - 1); strncat(buf, k->image->name, sizeof buf - strlen(buf) - 1); }
    return nr_str_utf8(buf);
}
static void* MonoType_get_BaseType(void* self) { NrClass* k = K(self); return (k->flags & NR_INTERFACE) || !k->parent ? 0 : nr_type_object(k->parent); }
static void* MonoType_GetElementType(void* self) { NrClass* k = K(self); return (k->flags & (NR_ARRAY | NR_POINTER)) ? nr_type_object(k->elem) : 0; }
static int32_t MonoType_GetArrayRank(void* self) { return K(self)->rank; }
static void* MonoType_get_DeclaringType(void* self) { NrClass* k = K(self); return k->nested_in ? nr_type_object(k->nested_in) : 0; }
static int32_t MonoType_get_attributes(void* t) { return (int32_t)K(t)->attrs; }
static uint8_t MonoType_IsByRefImpl(void* self) { return (uint8_t)nr_type_byref(self); }
static uint8_t MonoType_IsPointerImpl(void* self) { return (K(self)->flags & NR_POINTER) != 0; }
static uint8_t MonoType_IsCOMObjectImpl(void* self) { (void)self; return 0; }
static uint8_t MonoType_IsPrimitiveImpl(void* self)
{
    uint8_t t = K(self)->byval_arg.type;
    return (t >= 2 && t <= 0xd) || t == 0x18 || t == 0x19;
}
static uint8_t MonoType_false(void* self) { (void)self; return 0; }
static void* MonoType_null(void* self) { (void)self; return 0; }
static void* MonoType_GetGenericArguments(void* self)
{
    NrClass* k = K(self);
    int n = k->n_gargs;
    const char* tick = strchr(k->name, '`');
    /* A generic definition has parameters, not arguments; nothing models them,
     * so each one reads as System.Object. Only the count matters to callers
     * like MakeGenericType. */
    if (!n && tick && !(k->flags & NR_ARRAY)) n = atoi(tick + 1);
    void* a = new_array(nr_class_by_name("System", "Type"), n);
    for (int i = 0; i < n; i++) adata(a)[i] = nr_type_object(i < k->n_gargs ? k->gargs[i] : nr_k_object);
    return a;
}
static void* MonoType_GetInterfaces(void* self)
{
    NrClass* k = K(self);
    NrClass* seen[256];
    int n = 0;
    for (NrClass* c = k; c; c = c->parent)
        for (int i = 0; i < c->n_ifaces && n < 256; i++)
        {
            int dup = 0;
            for (int j = 0; j < n; j++) dup |= seen[j] == c->ifaces[i].iface;
            if (!dup) seen[n++] = c->ifaces[i].iface;
        }
    void* a = new_array(nr_class_by_name("System", "Type"), n);
    for (int i = 0; i < n; i++) adata(a)[i] = nr_type_object(seen[i]);
    return a;
}
static void MonoType_type_from_obj(void* t, void* o) { (void)t; (void)o; nr_throw_notsupported("MonoType.type_from_obj"); }
static void* MonoType_get_Assembly(void* self);
static void* MonoType_get_Module(void* self);

/* BindingFlags: Instance 4, Static 8, Public 16, NonPublic 32, DeclaredOnly 2, IgnoreCase 1 */
static int bind_ok(uint16_t attrs, int32_t bf, int inherited)
{
    int pub = (attrs & 7) == 6, stat = (attrs & 0x10) != 0;
    if (!((pub && (bf & 16)) || (!pub && (bf & 32)))) return 0;
    if (!((stat && (bf & 8)) || (!stat && (bf & 4)))) return 0;
    if (inherited && stat && !(bf & 64)) return 0;
    if (inherited && (attrs & 7) == 1) return 0;   /* private members aren't inherited */
    return 1;
}
static int name_eq(const char* a, void* s, int ic)
{
    if (!s) return 1;
    char* b = nr_utf8(s);
    int r = ic ? !_stricmp(a, b) : !strcmp(a, b);
    nr_free(b);
    return r;
}

static void* MonoType_GetMethodsByName(void* self, void* name, int32_t bf, uint8_t ic, void* reftype)
{
    (void)reftype;
    NrClass* k = K(self);
    void** tmp = 0; int n = 0, cap = 0;
    for (NrClass* c = k; c; c = (bf & 2) ? 0 : c->parent)
        for (int i = 0; i < c->n_methods; i++)
        {
            const NrMethodDesc* m = &c->methods[i];
            if (!m->name || m->mgargs || is_ctor(m) || !bind_ok(m->flags, bf, c != k) || !name_eq(m->name, name, ic)) continue;
            /* an override hides the base declaration */
            int hidden = 0;
            if (c != k && m->slot >= 0)
                for (int j = 0; j < n && !hidden; j++) { const NrMethodDesc* o = GETF(tmp[j], const NrMethodDesc*, k_monomethod, "mhandle"); hidden = o->slot == m->slot && !strcmp(o->name, m->name); }
            if (hidden) continue;
            if (n == cap) { cap = cap ? cap * 2 : 16; void** t2 = (void**)GC_MALLOC(sizeof(void*) * cap); if (tmp) memcpy(t2, tmp, sizeof(void*) * n); tmp = t2; }
            tmp[n++] = nr_method_object(m);
        }
    void* a = new_array(nr_class_by_name("System.Reflection", "MethodInfo"), n);
    if (n) memcpy(adata(a), tmp, sizeof(void*) * n);
    return a;
}
static void* MonoType_GetConstructors_internal(void* self, int32_t bf, void* reftype)
{
    (void)reftype;
    NrClass* k = K(self);
    int n = 0;
    for (int i = 0; i < k->n_methods; i++) if (!strcmp(k->methods[i].name, ".ctor") && bind_ok(k->methods[i].flags, bf, 0)) n++;
    void* a = new_array(nr_class_by_name("System.Reflection", "ConstructorInfo"), n);
    int j = 0;
    for (int i = 0; i < k->n_methods; i++) if (!strcmp(k->methods[i].name, ".ctor") && bind_ok(k->methods[i].flags, bf, 0)) adata(a)[j++] = nr_method_object(&k->methods[i]);
    return a;
}
static void* MonoType_GetFields_internal(void* self, int32_t bf, void* reftype)
{
    (void)reftype;
    NrClass* k = K(self);
    int n = 0;
    for (NrClass* c = k; c; c = (bf & 2) ? 0 : c->parent)
        for (int i = 0; i < c->n_fields; i++) if (bind_ok(c->fields[i].flags, bf, c != k)) n++;
    void* a = new_array(nr_class_by_name("System.Reflection", "FieldInfo"), n);
    int j = 0;
    for (NrClass* c = k; c; c = (bf & 2) ? 0 : c->parent)
        for (int i = 0; i < c->n_fields; i++) if (bind_ok(c->fields[i].flags, bf, c != k)) adata(a)[j++] = field_object(&c->fields[i]);
    return a;
}
static void* MonoType_GetField(void* self, void* name, int32_t bf)
{
    NrClass* k = K(self);
    for (NrClass* c = k; c; c = (bf & 2) ? 0 : c->parent)
        for (int i = 0; i < c->n_fields; i++)
            if (bind_ok(c->fields[i].flags, bf, c != k) && name_eq(c->fields[i].name, name, bf & 1)) return field_object(&c->fields[i]);
    return 0;
}
static uint16_t prop_attrs(const NrProp* p) { const NrMethodDesc* m = p->get ? p->get : p->set; return m ? m->flags : 0; }
static void* MonoType_GetPropertiesByName(void* self, void* name, int32_t bf, uint8_t ic, void* reftype)
{
    (void)reftype;
    NrClass* k = K(self);
    void* tmp[512]; int n = 0;
    for (NrClass* c = k; c && n < 512; c = (bf & 2) ? 0 : c->parent)
    {
        int np; const NrProp* ps = nr_props_of(c, &np);
        for (int i = 0; i < np && n < 512; i++)
        {
            if (!bind_ok(prop_attrs(&ps[i]), bf, c != k) || !name_eq(ps[i].name, name, ic)) continue;
            int dup = 0;
            for (int j = 0; j < n; j++) dup |= !strcmp(((const NrProp*)GETF(tmp[j], void*, k_monoproperty, "prop"))->name, ps[i].name);
            if (!dup) tmp[n++] = prop_object(&ps[i]);
        }
    }
    void* a = new_array(nr_class_by_name("System.Reflection", "PropertyInfo"), n);
    if (n) memcpy(adata(a), tmp, sizeof(void*) * n);
    return a;
}
static void* MonoType_GetEvents_internal(void* self, int32_t bf, void* reftype) { (void)self; (void)bf; (void)reftype; return new_array(nr_class_by_name("System.Reflection", "EventInfo"), 0); }
static void* MonoType_InternalGetEvent(void* self, void* n, int32_t bf) { (void)self; (void)n; (void)bf; return 0; }
static void* MonoType_GetNestedTypes(void* self, int32_t bf)
{
    (void)bf;
    NrClass* k = K(self);
    int n = 0;
    for (int i = 0; i < nr_n_classes; i++) if (nr_classes[i]->nested_in == k) n++;
    void* a = new_array(nr_class_by_name("System", "Type"), n);
    int j = 0;
    for (int i = 0; i < nr_n_classes; i++) if (nr_classes[i]->nested_in == k) adata(a)[j++] = nr_type_object(nr_classes[i]);
    return a;
}
static void* MonoType_GetNestedType(void* self, void* name, int32_t bf)
{
    NrClass* k = K(self);
    for (int i = 0; i < nr_n_classes; i++) if (nr_classes[i]->nested_in == k && name_eq(nr_classes[i]->name, name, bf & 1)) return nr_type_object(nr_classes[i]);
    return 0;
}
static void* MonoType_GetCorrespondingInflatedMethod(void* self, void* m) { (void)self; return m; }

/* Type.GetType(string): "Ns.Name", "Ns.Outer+Inner", optionally ", Assembly" */
static NrClass* class_from_name(const char* full)
{
    char buf[1024];
    strncpy(buf, full, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    char* comma = strchr(buf, ',');
    if (comma) *comma = 0;
    while (*buf && buf[strlen(buf) - 1] == ' ') buf[strlen(buf) - 1] = 0;
    int arr = 0;
    size_t L = strlen(buf);
    if (L > 2 && !strcmp(buf + L - 2, "[]")) { arr = 1; buf[L - 2] = 0; }
    char* plus = strchr(buf, '+');
    NrClass* k;
    if (plus)
    {
        *plus = 0;
        k = class_from_name(buf);
        char* rest = plus + 1;
        while (k && rest)
        {
            char* nx = strchr(rest, '+');
            if (nx) *nx++ = 0;
            NrClass* f = 0;
            for (int i = 0; i < nr_n_classes && !f; i++) if (nr_classes[i]->nested_in == k && !strcmp(nr_classes[i]->name, rest)) f = nr_classes[i];
            k = f; rest = nx;
        }
    }
    else
    {
        char* dot = strrchr(buf, '.');
        k = dot ? (*dot = 0, nr_class_by_name(buf, dot + 1)) : nr_class_by_name("", buf);
    }
    return k && arr ? nr_array_class(k, 1) : k;
}
/* compiled instance of def over args: exact, else the reference-type-shared one */
static NrClass* find_instance(NrClass* d, NrClass** args, int n)
{
    NrClass* best = 0;
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* c = nr_classes[i];
        if (!(c->flags & NR_GENERIC_INST) || c->n_gargs != n || c->image != d->image || strcmp(c->name, d->name) || strcmp(c->ns, d->ns)) continue;
        int exact = 1, shared = 1;
        for (int j = 0; j < n; j++)
        {
            exact &= c->gargs[j] == args[j];
            shared &= c->gargs[j] == args[j] || (nr_is_ref(args[j]) && nr_is_ref(c->gargs[j]) && nr_assignable(c->gargs[j], args[j]));
        }
        if (exact) return c;
        if (shared && !best) best = c;
    }
    return best;
}
/* "Ns.Gen`2[[A, Asm],[B, Asm]][], Asm" (or "[A,B]"): parse at *p, stop at an unmatched ']' or ',' */
static NrClass* parse_type(const char** p)
{
    char name[512]; int n = 0;
    const char* s = *p;
    while (*s && *s != '[' && *s != ']' && *s != ',' && n < 511) name[n++] = *s++;
    while (n && name[n - 1] == ' ') n--;
    name[n] = 0;
    NrClass* k = class_from_name(name);
    if (*s == '[' && s[1] != ']' && s[1] != ',')
    {
        NrClass* args[16]; int na = 0;
        s++;
        while (*s && *s != ']' && na < 16)
        {
            while (*s == ' ' || *s == ',') s++;
            if (*s == '[')
            {
                s++;
                args[na++] = parse_type(&s);
                int depth = 0;   /* skip ", Assembly, Version=..." up to this arg's ']' */
                while (*s && !(*s == ']' && depth == 0)) { if (*s == '[') depth++; else if (*s == ']') depth--; s++; }
                if (*s == ']') s++;
            }
            else args[na++] = parse_type(&s);
        }
        if (*s == ']') s++;
        int ok = k != 0;
        for (int i = 0; i < na; i++) ok &= args[i] != 0;
        k = ok ? find_instance(k, args, na) : 0;
    }
    while (s[0] == '[' && s[1] == ']') { if (k) k = nr_array_class(k, 1); s += 2; }
    *p = s;
    return k;
}
static void* Type_internal_from_name(void* name, uint8_t throw_err, uint8_t ic)
{
    (void)ic;
    char* n = nr_utf8(name);
    const char* q = n;
    NrClass* k = strchr(n, '[') ? parse_type(&q) : class_from_name(n);
    if (!k && throw_err) { char msg[1100]; _snprintf(msg, sizeof msg, "type %s not found", n); msg[sizeof msg - 1] = 0; nr_free(n); nr_throw_new("System", "TypeLoadException", msg); }
    nr_free(n);
    return k ? nr_type_object(k) : 0;
}

/* ---- Enum ---- */

static void* Enum_get_value(void* self) { NrClass* k = OBJ(self)->klass; return nr_box(k->elem, (char*)self + sizeof(NrObj)); }
static void* Enum_get_underlying_type(void* t) { return nr_type_object(K(t)->elem); }
static int64_t enum_bits(void* o)
{
    NrClass* k = OBJ(o)->klass;
    if (k->flags & NR_ENUM) k = k->elem;
    const char* p = (const char*)o + sizeof(NrObj);
    switch (k->byval_arg.type)
    {
    case 2: case 5: return *(uint8_t*)p;
    case 4: return *(int8_t*)p;
    case 3: case 7: return *(uint16_t*)p;
    case 6: return *(int16_t*)p;
    case 8: return *(int32_t*)p;
    case 9: return *(uint32_t*)p;
    default: return *(int64_t*)p;
    }
}
static int32_t Enum_compare_value_to(void* self, void* other)
{
    int64_t a = enum_bits(self), b = enum_bits(other);
    NrClass* u = OBJ(self)->klass->elem;
    if (u->byval_arg.type == 0xb) return (uint64_t)a < (uint64_t)b ? -1 : (uint64_t)a > (uint64_t)b;
    return a < b ? -1 : a > b;
}
static int32_t Enum_get_hashcode(void* self) { int64_t v = enum_bits(self); return (int32_t)v ^ (int32_t)(v >> 32); }
static void* Enum_ToObject(void* t, void* v)
{
    NrClass* k = K(t);
    int64_t bits = enum_bits(v);
    return nr_box(k, &bits);   /* little endian: the low bytes are the value */
}
static void Enum_get_enum_info(void* t, void** info)
{
    NrClass* k = K(t);
    NrClass* u = k->elem;
    int n = 0;
    for (int i = 0; i < k->n_fields; i++) if (k->fields[i].flags & 0x10) n++;
    void* vals = nr_newarr(nr_array_class(k, 1), n);   /* enum-typed: Format casts elements to Enum */
    void* names = nr_new_string_array(n);
    int j = 0;
    for (int i = 0; i < k->n_fields; i++)
    {
        const NrFieldDesc* f = &k->fields[i];
        if (!(f->flags & 0x10)) continue;
        if (f->rva) memcpy((char*)ARR(vals)->vector + (size_t)j * u->value_size, f->rva, (size_t)u->value_size);
        adata(names)[j] = nr_str_utf8(f->name);
        j++;
    }
    info[0] = nr_type_object(u);
    info[1] = vals;
    info[2] = names;
    info[3] = 0;
}

/* ---- Activator ---- */

static void* Activator_CreateInstanceInternal(void* t)
{
    NrClass* k = K(t);
    if (k->flags & (NR_INTERFACE | NR_ABSTRACT)) nr_throw_new("System", "MissingMethodException", "Cannot create an abstract class.");
    return nr_new(k);
}

/* ---- methods ---- */

static void MonoMethodInfo_get_method_info(intptr_t h, void** info)
{
    const NrMethodDesc* m = (const NrMethodDesc*)h;
    info[0] = nr_type_object(m->klass);
    info[1] = m->ret ? nr_type_object(m->ret) : 0;
    /* MonoMethodInfo { Type parent, ret; int attrs, iattrs, callconv; }: the ints follow two pointers */
    int32_t* ints = (int32_t*)&info[2];
    ints[0] = m->flags;
    ints[1] = m->iflags;
    ints[2] = (m->flags & 0x10) ? 1 : 0x21;
}
static void* MonoMethodInfo_get_parameter_info(intptr_t h, void* member)
{
    const NrMethodDesc* m = (const NrMethodDesc*)h;
    void* a = new_array(k_paraminfo, m->n_params);
    for (int i = 0; i < m->n_params; i++)
    {
        void* p = nr_new(k_paraminfo);
        NrClass* pt = m->params ? m->params[i] : 0;
        SETF(p, void*, k_paraminfo, "ClassImpl", pt ? nr_type_object(pt) : 0);
        SETF(p, void*, k_paraminfo, "MemberImpl", member);
        char nm[16]; _snprintf(nm, sizeof nm, "arg%d", i);
        SETF(p, void*, k_paraminfo, "NameImpl", nr_str_utf8(nm));
        SETF(p, int32_t, k_paraminfo, "PositionImpl", i);
        SETF(p, int32_t, k_paraminfo, "AttrsImpl", m->param_byref && m->param_byref[i] ? 2 : 0);
        adata(a)[i] = p;
    }
    return a;
}
static void* MonoMethodInfo_get_retval_marshal(intptr_t h) { (void)h; return 0; }
static void* MonoMethod_get_name(void* m) { return nr_str_utf8(GETF(m, const NrMethodDesc*, OBJ(m)->klass, "mhandle")->name); }
static void* MonoMethod_get_base_definition(void* m) { return m; }
static uint8_t MonoMethod_false(void* m) { (void)m; return 0; }
#define MH(o) GETF(o, const NrMethodDesc*, OBJ(o)->klass, "mhandle")
static uint8_t MonoMethod_get_IsGenericMethod(void* m) { return MH(m)->n_mgargs != 0; }
static uint8_t MonoMethod_get_IsGenericMethodDefinition(void* m) { return MH(m)->n_mgargs && !MH(m)->mgargs; }
static void* MonoMethod_GetGenericArguments(void* m)
{
    const NrMethodDesc* d = MH(m);
    void* a = new_array(nr_class_by_name("System", "Type"), d->n_mgargs);
    for (int i = 0; i < d->n_mgargs; i++) adata(a)[i] = nr_type_object(d->mgargs ? d->mgargs[i] : nr_k_object);
    return a;
}
static void* MonoMethod_GetGenericMethodDefinition_impl(void* m)
{
    const NrMethodDesc* d = MH(m);
    if (!d->n_mgargs) return 0;
    for (int i = 0; i < d->klass->n_methods; i++)
    {
        const NrMethodDesc* x = &d->klass->methods[i];
        if (x->token == d->token && x->n_mgargs && !x->mgargs) return nr_method_object(x);
    }
    return m;
}
/* Instances are compiled ahead of time. MakeGenericMethod takes the exact one, else
 * the closest compiled instance its reference-type arguments are assignable to
 * (the canonical <object,...> one the lifter adds for reflection targets).
 * ponytail: a shared body sees typeof(T) as its compiled argument, not the requested
 * one; a real per-call generic context is the upgrade if a title depends on it. */
static void* MonoMethod_MakeGenericMethod_impl(void* m, void* types)
{
    const NrMethodDesc* d = MH(m);
    int n = types ? (int)ARR(types)->max_length : 0;
    if (!d->n_mgargs || n != d->n_mgargs) nr_throw_new("System", "InvalidOperationException", "not a generic method definition");
    const NrMethodDesc* best = 0; int bestScore = -1;
    for (int i = 0; i < d->klass->n_methods; i++)
    {
        const NrMethodDesc* x = &d->klass->methods[i];
        if (x->token != d->token || !x->mgargs || x->n_mgargs != n) continue;
        int score = 0, ok = 1;
        for (int j = 0; j < n && ok; j++)
        {
            NrClass *want = K(adata(types)[j]), *have = x->mgargs[j];
            if (want == have) score += 2;
            else ok = have && nr_is_ref(want) && nr_is_ref(have) && nr_assignable(have, want);
        }
        if (ok && score > bestScore) { best = x; bestScore = score; }
    }
    if (best) return nr_method_object(best);
    char msg[300];
    _snprintf(msg, sizeof msg, "%s.%s::%s with these arguments was not compiled", d->klass->ns, d->klass->name, d->name);
    msg[sizeof msg - 1] = 0;
    nr_throw_notsupported(msg);
}
/* A generic virtual method's instance has no slot: find the receiver's own instance. */
static const NrMethodDesc* gvm_target(const NrMethodDesc* m, void* obj)
{
    if (!obj || !m->mgargs || !(m->flags & 0x40)) return m;
    const char* nm = strrchr(m->name, '.') ? strrchr(m->name, '.') + 1 : m->name;
    for (NrClass* c = OBJ(obj)->klass; c; c = c->parent)
        for (int i = 0; i < c->n_methods; i++)
        {
            const NrMethodDesc* x = &c->methods[i];
            if (!x->fn || !x->mgargs || x->n_mgargs != m->n_mgargs || x->n_params != m->n_params) continue;
            const char* xn = strrchr(x->name, '.') ? strrchr(x->name, '.') + 1 : x->name;
            if (strcmp(xn, nm)) continue;
            int ok = 1;
            for (int j = 0; j < m->n_mgargs && ok; j++) ok = x->mgargs[j] == m->mgargs[j];
            if (ok) return x;
        }
    return m;
}
static void* MonoMethod_GetDllImportAttribute(intptr_t h) { (void)h; return 0; }

/* A method of a generic class instance nothing called directly (reflection only, e.g.
 * a serializer calling List<T>.AddRange) runs the canonical instance's body: the same
 * class over object for every reference-type argument has the same layout and code.
 * The lifter compiles the public methods of those canonical instances. */
static const NrMethodDesc* canonical_method(const NrMethodDesc* m)
{
    NrClass* k = m->klass;
    if (m->fn || !(k->flags & NR_GENERIC_INST)) return m;
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* c = nr_classes[i];
        if (c == k || !(c->flags & NR_GENERIC_INST) || c->n_gargs != k->n_gargs || c->image != k->image
            || strcmp(c->name, k->name) || strcmp(c->ns, k->ns)) continue;
        int ok = 1;
        for (int j = 0; j < k->n_gargs && ok; j++)
            ok = nr_is_ref(k->gargs[j]) ? nr_is_ref(c->gargs[j]) && nr_assignable(c->gargs[j], k->gargs[j]) : c->gargs[j] == k->gargs[j];
        if (!ok) continue;
        for (int j = 0; j < c->n_methods; j++)
            if (c->methods[j].token == m->token && c->methods[j].fn && c->methods[j].n_params == m->n_params && !c->methods[j].mgargs)
                return &c->methods[j];
    }
    return m;
}

/* MethodBase.Invoke: args are boxed; value-type parameters get a pointer
 * into the box, byref parameters a pointer to the slot, written back. */
static void* invoke(void* mobj, void* obj, void* params, void** exc, int ctor)
{
    const NrMethodDesc* m = GETF(mobj, const NrMethodDesc*, OBJ(mobj)->klass, "mhandle");
    int n = m->n_params;
    void* argv[32];
    if (n > 32) nr_throw_notsupported("Invoke with more than 32 parameters");
    if ((params ? (int)ARR(params)->max_length : 0) != n) nr_throw_new("System.Reflection", "TargetParameterCountException", "Parameter count mismatch.");
    for (int i = 0; i < n; i++)
    {
        void** slot = &adata(params)[i];
        NrClass* pt = m->params ? m->params[i] : 0;
        int byref = m->param_byref && m->param_byref[i];
        if (pt && !nr_is_ref(pt))
        {
            if (!*slot) *slot = nr_box(pt, &(int64_t[8]){ 0 });
            argv[i] = (char*)*slot + sizeof(NrObj);
        }
        else argv[i] = byref ? (void*)slot : *slot;
    }
    if (ctor && !obj)
    {
        NrClass* want = m->klass;   /* allocate what was asked for; the body may be shared */
        if (!m->fn) m = canonical_method(m);
        void* o = nr_new(want);
        nr_invoke(m, (want->flags & NR_VALUETYPE) ? (char*)o + sizeof(NrObj) : o, argv, exc);
        return *exc ? 0 : o;
    }
    if (m->mgargs && !ctor) m = gvm_target(m, obj);
    if (!m->fn) m = canonical_method(m);
    const NrMethodDesc* target = m;
    void* fn = nr_virtual_desc(m, obj, &target);
    if (fn != m->fn)
    {
        NrMethodDesc tmp = *m;
        tmp.fn = fn;
        return nr_invoke(&tmp, obj, argv, exc);
    }
    return nr_invoke(m, obj, argv, exc);
}
static void* MonoMethod_InternalInvoke(void* self, void* obj, void* params, void** exc) { return invoke(self, obj, params, exc, 0); }
static void* MonoCMethod_InternalInvoke(void* self, void* obj, void* params, void** exc) { return invoke(self, obj, params, exc, 1); }
static void* MethodBase_GetMethodFromHandleInternalType(intptr_t h, intptr_t t) { (void)t; return nr_method_object((const NrMethodDesc*)h); }
static void* MethodBase_GetCurrentMethod(void) { return 0; }

/* ---- fields ---- */

static void* field_ptr(const NrFieldDesc* f, void* obj)
{
    if (f->flags & 0x10) { if (f->parent->cctor_state != 2) nr_run_cctor(f->parent); return (char*)f->parent->statics + f->offset; }
    if (!obj) nr_throw_new("System.Reflection", "TargetException", "Non-static field requires a target.");
    return (char*)obj + f->offset;
}
static void* MonoField_GetValueInternal(void* self, void* obj)
{
    const NrFieldDesc* f = GETF(self, const NrFieldDesc*, k_monofield, "fhandle");
    if (f->flags & 0x40)  /* literal */
    {
        if (f->type == nr_k_string) return f->rva ? nr_str_utf16((const uint16_t*)f->rva, f->rva_size / 2) : 0;
        return f->rva ? nr_box(f->type, f->rva) : 0;
    }
    void* p = field_ptr(f, obj);
    return nr_is_ref(f->type) ? *(void**)p : nr_box(f->type, p);
}
static void MonoField_SetValueInternal(void* fi, void* obj, void* v)
{
    const NrFieldDesc* f = GETF(fi, const NrFieldDesc*, k_monofield, "fhandle");
    void* p = field_ptr(f, obj);
    if (nr_is_ref(f->type)) *(void**)p = v;
    else if (v) memcpy(p, (char*)v + sizeof(NrObj), (size_t)f->type->value_size);
    else memset(p, 0, (size_t)f->type->value_size);
}
static void* MonoField_GetParentType(void* self, uint8_t declaring) { (void)declaring; return nr_type_object(GETF(self, NrClass*, k_monofield, "klass")); }
static int32_t MonoField_GetFieldOffset(void* self) { return GETF(self, const NrFieldDesc*, k_monofield, "fhandle")->offset - (int32_t)sizeof(NrObj); }
static void* MonoField_GetRawConstantValue(void* self) { return MonoField_GetValueInternal(self, 0); }
static void* FieldInfo_internal_from_handle_type(intptr_t h, intptr_t t) { (void)t; return field_object((const NrFieldDesc*)h); }

/* ---- properties ---- */

static void MonoPropertyInfo_get_property_info(void* prop, void** info, int32_t req)
{
    (void)req;
    const NrProp* p = GETF(prop, const NrProp*, k_monoproperty, "prop");
    info[0] = nr_type_object(p->klass);
    info[1] = nr_str_utf8(p->name);
    info[2] = p->get ? nr_method_object(p->get) : 0;
    info[3] = p->set ? nr_method_object(p->set) : 0;
    *(int32_t*)&info[4] = 0;   /* attrs, after four pointers */
}
static void* MonoPropertyInfo_GetTypeModifiers(void* prop, uint8_t opt) { (void)prop; (void)opt; return 0; }

/* ---- custom attributes ----
 * The generated tables hold one factory per attribute usage; objects are
 * built fresh on every query, as mono did. Properties carry none yet. */
void nr_attrs_of(void* provider, const NrAttr** a, int* n)
{
    *a = 0; *n = 0;
    if (!provider) return;
    NrClass* k = OBJ(provider)->klass;
    if (k == nr_k_monotype) { NrClass* c = K(provider); *a = c->cattrs; *n = c->n_cattrs; }
    else if (k == k_monomethod || k == k_monocmethod) { const NrMethodDesc* m = GETF(provider, const NrMethodDesc*, k, "mhandle"); *a = m->cattrs; *n = m->n_cattrs; }
    else if (k == k_monofield) { const NrFieldDesc* f = GETF(provider, const NrFieldDesc*, k, "fhandle"); *a = f->cattrs; *n = f->n_cattrs; }
}
static void* MonoCustomAttrs_GetCustomAttributesInternal(void* obj, void* type, uint8_t pseudo)
{
    (void)pseudo;
    const NrAttr* a; int n;
    nr_attrs_of(obj, &a, &n);
    NrClass* want = type ? K(type) : 0;
    int c = 0;
    for (int i = 0; i < n; i++) if (!want || nr_assignable(want, a[i].klass)) c++;
    void* arr = nr_newarr(nr_k_objarr, c);
    int j = 0;
    for (int i = 0; i < n; i++) if (!want || nr_assignable(want, a[i].klass)) adata(arr)[j++] = a[i].make();
    return arr;
}
static uint8_t MonoCustomAttrs_IsDefinedInternal(void* obj, void* type)
{
    const NrAttr* a; int n;
    nr_attrs_of(obj, &a, &n);
    NrClass* want = K(type);
    for (int i = 0; i < n; i++) if (nr_assignable(want, a[i].klass)) return 1;
    return 0;
}

/* ---- assemblies ---- */

static void* assembly_object(NrImage* img)
{
    if (!img) return 0;
    if (img->assembly_obj) return img->assembly_obj;
    void* a = nr_new(k_assembly);
    SETF(a, NrImage*, k_assembly, "_mono_assembly", img);
    nr_gchandle_new(a, 0);
    img->assembly_obj = a;
    return a;
}
void* nr_assembly_object(NrImage* img) { return assembly_object(img); }
static NrImage* asm_image(void* a) { return GETF(a, NrImage*, k_assembly, "_mono_assembly"); }
static void* MonoType_get_Assembly(void* self) { return assembly_object(K(self)->image); }
static void* MonoType_get_Module(void* self) { (void)self; return 0; }
static void* Assembly_GetExecutingAssembly(void) { return assembly_object(nr_images[0]); }
static void* Assembly_get_fullname(void* self)
{
    char buf[512];
    _snprintf(buf, sizeof buf, "%s, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null", asm_image(self)->name);
    buf[sizeof buf - 1] = 0;
    return nr_str_utf8(buf);
}
static void* Assembly_get_location(void* self)
{
    char buf[MAX_PATH * 2];
    GetModuleFileNameA(0, buf, MAX_PATH);
    char* s = strrchr(buf, '\\');
    if (s) s[1] = 0;
    strncat(buf, asm_image(self)->file, sizeof buf - strlen(buf) - 1);
    return nr_str_utf8(buf);
}
static void* Assembly_get_code_base(void* self, uint8_t esc) { (void)esc; return Assembly_get_location(self); }
static void* Assembly_GetTypes(void* self, uint8_t exported)
{
    (void)exported;
    NrImage* img = asm_image(self);
    int n = 0;
    for (int i = 0; i < nr_n_classes; i++) { NrClass* k = nr_classes[i]; if (k->image == img && !(k->flags & (NR_ARRAY | NR_POINTER | NR_GENERIC_INST))) n++; }
    void* a = new_array(nr_class_by_name("System", "Type"), n);
    int j = 0;
    for (int i = 0; i < nr_n_classes; i++) { NrClass* k = nr_classes[i]; if (k->image == img && !(k->flags & (NR_ARRAY | NR_POINTER | NR_GENERIC_INST))) adata(a)[j++] = nr_type_object(k); }
    return a;
}
static void* Assembly_InternalGetType(void* self, void* module, void* name, uint8_t throw_err, uint8_t ic)
{
    (void)module; (void)ic;
    char* n = nr_utf8(name);
    NrClass* k = class_from_name(n);
    if (k && self && k->image != asm_image(self)) k = 0;
    if (!k && throw_err) { char msg[600]; _snprintf(msg, sizeof msg, "type %s not found", n); msg[sizeof msg - 1] = 0; nr_free(n); nr_throw_new("System", "TypeLoadException", msg); }
    nr_free(n);
    return k ? nr_type_object(k) : 0;
}
static void* Assembly_load(void* name)
{
    char* n = nr_utf8(name);
    char* c = strchr(n, ',');
    if (c) *c = 0;
    void* r = 0;
    for (int i = 0; i < nr_n_images; i++) if (!_stricmp(nr_images[i]->name, n)) r = assembly_object(nr_images[i]);
    nr_free(n);
    return r;
}
static void* AppDomain_LoadAssembly(void* self, void* name, void* ev, uint8_t refonly) { (void)self; (void)ev; (void)refonly; return Assembly_load(name); }
static void* AppDomain_GetAssemblies(void* self, uint8_t refonly)
{
    (void)self; (void)refonly;
    void* a = new_array(nr_class_by_name("System.Reflection", "Assembly"), nr_n_images);
    for (int i = 0; i < nr_n_images; i++) adata(a)[i] = assembly_object(nr_images[i]);
    return a;
}
static uint8_t Assembly_get_ReflectionOnly(void* self) { (void)self; return 0; }
static void* Assembly_null1(void* a) { (void)a; return 0; }
static void* Assembly_GetManifestResourceNames(void* self) { (void)self; return nr_new_string_array(0); }
static intptr_t Assembly_GetManifestResourceInternal(void* self, void* name, int32_t* size, void** module) { (void)self; (void)name; *size = 0; *module = 0; return 0; }
static void Assembly_FillName(void* asm_obj, void* aname)
{
    NrClass* k = OBJ(aname)->klass;
    NrImage* img = asm_image(asm_obj);
    SETF(aname, void*, k, "name", nr_str_utf8(img->name));
    /* the version comes from the full name, "X, Version=a.b.c.d, ..." */
    int v[4] = { 0, 0, 0, 0 };
    const char* p = img->fullname ? strstr(img->fullname, "Version=") : 0;
    if (p) sscanf(p + 8, "%d.%d.%d.%d", &v[0], &v[1], &v[2], &v[3]);
    SETF(aname, int32_t, k, "major", v[0]); SETF(aname, int32_t, k, "minor", v[1]);
    SETF(aname, int32_t, k, "build", v[2]); SETF(aname, int32_t, k, "revision", v[3]);
    NrClass* vk = nr_class_by_name("System", "Version");
    if (vk)
    {
        void* ver = nr_new(vk);
        SETF(ver, int32_t, vk, "_Major", v[0]); SETF(ver, int32_t, vk, "_Minor", v[1]);
        SETF(ver, int32_t, vk, "_Build", v[2]); SETF(ver, int32_t, vk, "_Revision", v[3]);
        SETF(aname, void*, k, "version", ver);
    }
}
static void* Assembly_GetReferencedAssemblies(void* self) { (void)self; return new_array(nr_class_by_name("System.Reflection", "AssemblyName"), 0); }
static void* Assembly_InternalImageRuntimeVersion(void* self) { (void)self; return nr_str_utf8("v2.0.50727"); }
static int32_t MemberInfo_get_MetadataToken(void* self) { (void)self; return 0; }
static void* StackTrace_get_trace(void* exc, int32_t skip, uint8_t need_file) { (void)exc; (void)skip; (void)need_file; return new_array(nr_class_by_name("System.Diagnostics", "StackFrame"), 0); }
/* No managed frame records: report that there's no frame. */
static uint8_t StackFrame_get_frame_info(int32_t skip, uint8_t need_file, void** method, int32_t* il, int32_t* native, void** file, int32_t* line, int32_t* col)
{
    (void)skip; (void)need_file;
    *method = 0; *il = 0; *native = 0; *file = 0; *line = 0; *col = 0;
    return 0;
}
static void* Delegate_CreateDelegate_internal(void* type, void* target, void* mi, uint8_t throw_err)
{
    (void)throw_err;
    NrClass* dk = K(type);
    const NrMethodDesc* m = GETF(mi, const NrMethodDesc*, OBJ(mi)->klass, "mhandle");
    void* d = nr_new(dk);
    const NrMethodDesc* t = m;
    void* fn = nr_virtual_desc(m, target, &t);
    nr_delegate_init(d, target, fn, m);
    return d;
}
static void Delegate_SetMulticastInvoke(void* self) { (void)self; }


/* ==== mono 4.x reflection (Unity 2017+) ====
 * Type objects are System.RuntimeType. Member queries return GPtrArrays of
 * native handles (MonoMethod*, MonoProperty*, ...), which the class library
 * turns into objects through the *_from_handle icalls. */
#if NR_CORLIB4
typedef struct GPtrArr { void** pdata; uint32_t len; } GPtrArr;
static GPtrArr* gpa_new(void) { GPtrArr* a = (GPtrArr*)calloc(1, sizeof *a); return a; }
static void gpa_add(GPtrArr* a, const void* p)
{
    if ((a->len & 15) == 0) a->pdata = (void**)realloc(a->pdata, sizeof(void*) * (a->len + 16));
    a->pdata[a->len++] = (void*)p;
}
static int name_eq8(const char* a, intptr_t utf8, int ic)
{
    if (!utf8) return 1;
    return ic ? !_stricmp(a, (const char*)utf8) : !strcmp(a, (const char*)utf8);
}

static intptr_t RT_GetMethodsByName_native(void* self, intptr_t name, int32_t bf, uint8_t ic)
{
    NrClass* k = K(self);
    GPtrArr* a = gpa_new();
    for (NrClass* c = k; c; c = (bf & 2) ? 0 : c->parent)
        for (int i = 0; i < c->n_methods; i++)
        {
            const NrMethodDesc* m = &c->methods[i];
            /* generic method instances are not members: reflection sees the definition */
            if (!m->name || m->mgargs || is_ctor(m) || !bind_ok(m->flags, bf, c != k) || !name_eq8(m->name, name, ic)) continue;
            int hidden = 0;
            if (c != k && m->slot >= 0)
                for (uint32_t j = 0; j < a->len && !hidden; j++) { const NrMethodDesc* o = (const NrMethodDesc*)a->pdata[j]; hidden = o->slot == m->slot && !strcmp(o->name, m->name); }
            if (!hidden) gpa_add(a, m);
        }
    return (intptr_t)a;
}
static intptr_t RT_GetConstructors_native(void* self, int32_t bf)
{
    NrClass* k = K(self);
    GPtrArr* a = gpa_new();
    for (int i = 0; i < k->n_methods; i++) if (!strcmp(k->methods[i].name, ".ctor") && bind_ok(k->methods[i].flags, bf, 0)) gpa_add(a, &k->methods[i]);
    return (intptr_t)a;
}
static intptr_t RT_GetFields_native(void* self, intptr_t name, int32_t bf)
{
    NrClass* k = K(self);
    GPtrArr* a = gpa_new();
    for (NrClass* c = k; c; c = (bf & 2) ? 0 : c->parent)
        for (int i = 0; i < c->n_fields; i++)
            if (bind_ok(c->fields[i].flags, bf, c != k) && name_eq8(c->fields[i].name, name, bf & 1)) gpa_add(a, &c->fields[i]);
    return (intptr_t)a;
}
static intptr_t RT_GetPropertiesByName_native(void* self, intptr_t name, int32_t bf, uint8_t ic)
{
    NrClass* k = K(self);
    GPtrArr* a = gpa_new();
    for (NrClass* c = k; c; c = (bf & 2) ? 0 : c->parent)
    {
        int np; const NrProp* ps = nr_props_of(c, &np);
        for (int i = 0; i < np; i++)
        {
            if (!bind_ok(prop_attrs(&ps[i]), bf, c != k) || !name_eq8(ps[i].name, name, ic)) continue;
            int dup = 0;
            for (uint32_t j = 0; j < a->len; j++) dup |= !strcmp(((const NrProp*)a->pdata[j])->name, ps[i].name);
            if (!dup) gpa_add(a, &ps[i]);
        }
    }
    return (intptr_t)a;
}
static intptr_t RT_GetEvents_native(void* self, intptr_t name, int32_t bf) { (void)self; (void)name; (void)bf; return (intptr_t)gpa_new(); }
static intptr_t RT_GetNestedTypes_native(void* self, intptr_t name, int32_t bf)
{
    NrClass* k = K(self);
    GPtrArr* a = gpa_new();
    for (int i = 0; i < nr_n_classes; i++)
    {
        NrClass* c = nr_classes[i];
        if (c->nested_in != k || !name_eq8(c->name, name, bf & 1)) continue;
        int pub = (c->attrs & 7) == 2;   /* NestedPublic */
        if ((pub && (bf & 16)) || (!pub && (bf & 32))) gpa_add(a, &c->byval_arg);
    }
    return (intptr_t)a;
}
static void* MethodBase_GetMethodFromHandleInternalType_native(intptr_t h, intptr_t t, uint8_t gc) { (void)t; (void)gc; return nr_method_object((const NrMethodDesc*)h); }
static void* PropertyInfo_internal_from_handle_type(intptr_t h, intptr_t t) { (void)t; return h ? prop_object((const NrProp*)h) : 0; }
static void* EventInfo_internal_from_handle_type(intptr_t h, intptr_t t) { (void)h; (void)t; return 0; }

/* RuntimeTypeHandle: statics over a RuntimeType */
static int32_t RTH_GetAttributes(void* t) { return (int32_t)K(t)->attrs; }
static void* RTH_GetBaseType(void* t) { return MonoType_get_BaseType(t); }
static void* RTH_GetElementType(void* t) { return MonoType_GetElementType(t); }
static int32_t RTH_GetArrayRank(void* t) { return K(t)->rank; }
static int32_t RTH_GetMetadataToken(void* t) { return (int32_t)K(t)->token; }
static uint8_t RTH_HasInstantiation(void* t) { return (K(t)->flags & NR_GENERIC_INST) != 0; }
static uint8_t RTH_IsGenericTypeDefinition(void* t) { return Type_get_IsGenericTypeDefinition(t); }
static uint8_t RTH_false(void* t) { (void)t; return 0; }
static intptr_t RTH_GetGenericParameterInfo(void* t) { (void)t; return 0; }
static void* RTH_GetGenericTypeDefinition_impl(void* t) { return Type_GetGenericTypeDefinition_impl(t); }
static uint8_t RTH_IsArray(void* t) { return (K(t)->flags & NR_ARRAY) != 0; }
static uint8_t RTH_IsByRef(void* t) { return (uint8_t)nr_type_byref(t); }
static uint8_t RTH_IsPointer(void* t) { return (K(t)->flags & NR_POINTER) != 0; }
static uint8_t RTH_IsPrimitive(void* t) { return MonoType_IsPrimitiveImpl(t); }
static uint8_t RTH_HasReferences(void* t) { NrClass* k = K(t); return nr_is_ref(k) || (k->flags & NR_HAS_REFS) != 0; }
static uint8_t RTH_IsInstanceOfType(void* t, void* o) { return nr_isinst(o, K(t)) != 0; }
static uint8_t RTH_type_is_assignable_from(void* a, void* b) { return nr_assignable(K(a), K(b)); }
static int32_t RTH_GetCorElementType(void* t) { return K(t)->byval_arg.type; }
static void* RTH_GetAssembly(void* t) { return assembly_object(K(t)->image); }
static void* module_object(NrImage* img);
static void* RTH_GetModule(void* t) { return module_object(K(t)->image); }

/* RuntimeType instance and static icalls */
static void* RT_GetGenericArgumentsInternal(void* self, uint8_t runtime)
{
    void* a = MonoType_GetGenericArguments(self);
    if (!runtime) return a;
    /* the runtime array is RuntimeType[]: the class library casts it */
    int n = (int)ARR(a)->max_length;
    void* r = new_array(nr_k_monotype, n);
    memcpy(adata(r), adata(a), sizeof(void*) * (size_t)n);
    return r;
}
static int32_t RT_GetGenericParameterPosition(void* self) { (void)self; return -1; }
static void* RT_MakePointerType(void* t) { (void)t; nr_throw_notsupported("Type.MakePointerType"); }
static int32_t RT_GetTypeCodeImplInternal(void* t) { return Type_GetTypeCodeInternal(t); }
static void* RT_CreateInstanceInternal(void* t) { return Activator_CreateInstanceInternal(t); }
static int32_t RT_security_level(void* self) { (void)self; return 0; }
static uint8_t RT_static_false(void* t) { (void)t; return 0; }
static void RT_GetInterfaceMapData(void* t, void* iface, void** targets, void** methods)
{
    NrClass* k = K(t); NrClass* ik = K(iface);
    NrClass* mk = nr_class_by_name("System.Reflection", "MethodInfo");
    int n = 0;
    for (int i = 0; i < ik->n_methods; i++) if (ik->methods[i].slot >= 0) n++;
    void* ta = new_array(mk, n); void* ma = new_array(mk, n);
    int off = -1;
    for (NrClass* c = k; c && off < 0; c = c->parent)
        for (int i = 0; i < c->n_ifaces; i++) if (c->ifaces[i].iface == ik) { off = c->ifaces[i].offset; break; }
    int j = 0;
    for (int i = 0; i < ik->n_methods; i++)
    {
        const NrMethodDesc* im = &ik->methods[i];
        if (im->slot < 0) continue;
        adata(ma)[j] = nr_method_object(im);
        void* fn = off >= 0 ? k->vtable[off + im->slot] : 0;
        for (NrClass* c = k; c && fn && !adata(ta)[j]; c = c->parent)
            for (int q = 0; q < c->n_methods; q++) if (c->methods[q].fn == fn) { adata(ta)[j] = nr_method_object(&c->methods[q]); break; }
        j++;
    }
    *targets = ta; *methods = ma;
}

/* members */
static int32_t MonoMethodInfo_get_method_attributes(intptr_t h) { return ((const NrMethodDesc*)h)->flags; }
static void* MonoMethod_get_name4(void* m) { return MonoMethod_get_name(m); }
static void* MonoMethod_self(void* m) { return m; }
static void* MonoMethod_get_base_method(void* m, uint8_t definition) { (void)definition; return m; }
static int32_t MonoMethod_GetPInvoke(void* m, int32_t* flags, void** entry, void** dll) { (void)m; *flags = 0; *entry = 0; *dll = 0; return 0; }
static void* MonoField_ResolveType(void* self) { return nr_type_object(GETF(self, const NrFieldDesc*, k_monofield, "fhandle")->type); }
static void* empty_types(void) { return new_array(nr_class_by_name("System", "Type"), 0); }
static void* FieldInfo_GetTypeModifiers(void* self, uint8_t opt) { (void)self; (void)opt; return empty_types(); }
static void* FieldInfo_get_marshal_info(void* self) { (void)self; return 0; }
static int32_t ParameterInfo_GetMetadataToken(void* self) { (void)self; return 0x08000000; }
static void* ParameterInfo_GetTypeModifiers(void* self, uint8_t opt) { (void)self; (void)opt; return empty_types(); }
static int32_t Module_get_MetadataToken(void* m) { (void)m; return 0x00000001; }
static void* MonoCustomAttrs_GetCustomAttributesDataInternal(void* obj) { (void)obj; return new_array(nr_class_by_name("System.Reflection", "CustomAttributeData"), 0); }
static void* MonoPropertyInfo_get_default_value(void* p) { (void)p; return 0; }

/* MonoPropertyInfo { Type parent; Type declaring_type; string name; MethodInfo get, set; PropertyAttributes attrs; } */
static void MonoPropertyInfo_get_property_info4(void* prop, void** info, int32_t req)
{
    (void)req;
    const NrProp* p = GETF(prop, const NrProp*, k_monoproperty, "prop");
    info[0] = nr_type_object(p->klass);
    info[1] = nr_type_object(p->klass);
    info[2] = nr_str_utf8(p->name);
    info[3] = p->get ? nr_method_object(p->get) : 0;
    info[4] = p->set ? nr_method_object(p->set) : 0;
    ((int32_t*)&info[5])[0] = 0;
}

/* assemblies and modules */
typedef struct MonoAsmName {
    const char* name; const char* culture; const char* hash_value; const uint8_t* public_key;
    uint8_t public_key_token[17]; uint32_t hash_alg, hash_len, flags;
    uint16_t major, minor, build, revision, arch;
} MonoAsmName;
static void asm_version(const char* full, uint16_t* v)
{
    int x[4] = { 0, 0, 0, 0 };
    const char* p = full ? strstr(full, "Version=") : 0;
    if (p) sscanf(p + 8, "%d.%d.%d.%d", &x[0], &x[1], &x[2], &x[3]);
    for (int i = 0; i < 4; i++) v[i] = (uint16_t)x[i];
}
static void* AssemblyName_GetNativeName(intptr_t asm_ptr)
{
    NrImage* img = (NrImage*)asm_ptr;
    if (!img) return 0;
    void* c = cache_get((char*)img + 1);
    if (c) return c;
    MonoAsmName* n = (MonoAsmName*)calloc(1, sizeof *n);
    n->name = img->name; n->culture = "";
    uint16_t v[4]; asm_version(img->fullname, v);
    n->major = v[0]; n->minor = v[1]; n->build = v[2]; n->revision = v[3];
    const char* t = img->fullname ? strstr(img->fullname, "PublicKeyToken=") : 0;
    if (t && strncmp(t + 15, "null", 4)) memcpy(n->public_key_token, t + 15, 16);
    return cache_put((char*)img + 1, n);
}
/* "Name, Version=1.2.3.4, Culture=neutral, PublicKeyToken=abcd..." */
static uint8_t AssemblyName_ParseAssemblyName(intptr_t utf8, MonoAsmName* out, uint8_t* has_ver, uint8_t* has_tok)
{
    memset(out, 0, sizeof *out);
    *has_ver = 0; *has_tok = 0;
    const char* s = (const char*)utf8;
    if (!s || !*s) return 0;
    const char* c = strchr(s, ',');
    size_t n = c ? (size_t)(c - s) : strlen(s);
    while (n && s[n - 1] == ' ') n--;
    char* nm = (char*)malloc(n + 1); memcpy(nm, s, n); nm[n] = 0;
    out->name = nm;
    if (strstr(s, "Version=")) { uint16_t v[4]; asm_version(s, v); out->major = v[0]; out->minor = v[1]; out->build = v[2]; out->revision = v[3]; *has_ver = 1; }
    const char* t = strstr(s, "PublicKeyToken=");
    if (t) { *has_tok = 1; if (strncmp(t + 15, "null", 4)) memcpy(out->public_key_token, t + 15, 16); }
    const char* cu = strstr(s, "Culture=");
    if (cu) { const char* e = strchr(cu, ','); size_t l = e ? (size_t)(e - cu - 8) : strlen(cu + 8); char* x = (char*)malloc(l + 1); memcpy(x, cu + 8, l); x[l] = 0; if (!_stricmp(x, "neutral")) x[0] = 0; out->culture = x; }
    return 1;
}
static void RuntimeMarshal_FreeAssemblyName(MonoAsmName* n, uint8_t free_struct)
{
    free((void*)n->name); free((void*)n->culture);
    n->name = 0; n->culture = 0;
    if (free_struct) free(n);
}
static void AssemblyName_get_public_token(uint8_t* token, uint8_t* pk, int32_t len) { (void)pk; (void)len; memset(token, 0, 8); }

static void* module_object(NrImage* img)
{
    if (!img || !k_module) return 0;
    void* m = cache_get((char*)img + 2);
    if (m) return m;
    m = nr_new(k_module);
    SETF(m, NrImage*, k_module, "_impl", img);
    SETF(m, void*, k_module, "assembly", assembly_object(img));
    SETF(m, void*, k_module, "name", nr_str_utf8(img->file));
    SETF(m, void*, k_module, "scopename", nr_str_utf8(img->file));
    SETF(m, void*, k_module, "fqname", Assembly_get_location(assembly_object(img)));
    return cache_put((char*)img + 2, m);
}
static void* Assembly_GetManifestModuleInternal(void* self) { return module_object(asm_image(self)); }
static void* Assembly_GetModulesInternal(void* self)
{
    void* a = new_array(nr_class_by_name("System.Reflection", "Module"), 1);
    adata(a)[0] = module_object(asm_image(self));
    return a;
}
static void* Assembly_GetFilesInternal(void* self, void* name, uint8_t res) { (void)self; (void)name; (void)res; return 0; }
static void* Assembly_GetAotId(void) { return 0; }
static intptr_t Assembly_InternalGetReferencedAssemblies(void* a) { (void)a; return (intptr_t)gpa_new(); }
static uint8_t Assembly_GetManifestResourceInfoInternal(void* self, void* name, void* info) { (void)self; (void)name; (void)info; return 0; }
static void* Assembly_load_with_partial_name(void* name, void* ev) { (void)ev; return Assembly_load(name); }
static uint8_t Assembly_LoadPermissions(void* a, intptr_t* m, int32_t* ml, intptr_t* o, int32_t* ol, intptr_t* r, int32_t* rl) { (void)a; *m = 0; *ml = 0; *o = 0; *ol = 0; *r = 0; *rl = 0; return 0; }
static uint8_t Assembly_get_global_assembly_cache(void* self) { (void)self; return 0; }
static void* AppDomain_LoadAssemblyRaw(void* self, void* raw, void* sym, void* ev, uint8_t refonly) { (void)self; (void)raw; (void)sym; (void)ev; (void)refonly; nr_throw_notsupported("Assembly.Load(byte[])"); }
static void* MissingMemberException_FormatSignature(void* sig) { (void)sig; return nr_str_utf8("?"); }
static void* RuntimeClassHandle_GetTypeFromClass(NrClass* k) { return k ? &k->byval_arg : 0; }

static void register_reflect4(void)
{
    k_paraminfo = nr_class_by_name("System.Reflection", "MonoParameterInfo");
    k_assembly = nr_class_by_name("System.Reflection", "MonoAssembly");
    k_module = nr_class_by_name("System.Reflection", "MonoModule");
    static const NrIcall t[] = {
        { "System.RuntimeType::GetMethodsByName_native", RT_GetMethodsByName_native },
        { "System.RuntimeType::GetConstructors_native", RT_GetConstructors_native },
        { "System.RuntimeType::GetFields_native", RT_GetFields_native },
        { "System.RuntimeType::GetPropertiesByName_native", RT_GetPropertiesByName_native },
        { "System.RuntimeType::GetEvents_native", RT_GetEvents_native },
        { "System.RuntimeType::GetNestedTypes_native", RT_GetNestedTypes_native },
        { "System.RuntimeType::get_Name", MonoType_get_Name },
        { "System.RuntimeType::get_Namespace", MonoType_get_Namespace },
        { "System.RuntimeType::getFullName", MonoType_getFullName },
        { "System.RuntimeType::get_DeclaringType", MonoType_get_DeclaringType },
        { "System.RuntimeType::get_DeclaringMethod", MonoType_null },
        { "System.RuntimeType::GetInterfaces", MonoType_GetInterfaces },
        { "System.RuntimeType::GetGenericArgumentsInternal", RT_GetGenericArgumentsInternal },
        { "System.RuntimeType::GetGenericParameterPosition", RT_GetGenericParameterPosition },
        { "System.RuntimeType::make_array_type", Type_make_array_type },
        { "System.RuntimeType::make_byref_type", Type_make_byref_type },
        { "System.RuntimeType::MakePointerType", RT_MakePointerType },
        { "System.RuntimeType::MakeGenericType", Type_MakeGenericType },
        { "System.RuntimeType::GetPacking", Type_GetPacking },
        { "System.RuntimeType::GetTypeCodeImplInternal", RT_GetTypeCodeImplInternal },
        { "System.RuntimeType::CreateInstanceInternal", RT_CreateInstanceInternal },
        { "System.RuntimeType::GetCorrespondingInflatedMethod", MonoType_GetCorrespondingInflatedMethod },
        { "System.RuntimeType::GetCorrespondingInflatedConstructor", MonoType_GetCorrespondingInflatedMethod },
        { "System.RuntimeType::get_core_clr_security_level", RT_security_level },
        { "System.RuntimeType::IsWindowsRuntimeObjectType", RT_static_false },
        { "System.RuntimeType::IsTypeExportedToWindowsRuntime", RT_static_false },
        { "System.RuntimeType::GetInterfaceMapData", RT_GetInterfaceMapData },
        { "System.RuntimeTypeHandle::GetAttributes", RTH_GetAttributes },
        { "System.RuntimeTypeHandle::GetBaseType", RTH_GetBaseType },
        { "System.RuntimeTypeHandle::GetElementType", RTH_GetElementType },
        { "System.RuntimeTypeHandle::GetArrayRank", RTH_GetArrayRank },
        { "System.RuntimeTypeHandle::GetMetadataToken", RTH_GetMetadataToken },
        { "System.RuntimeTypeHandle::HasInstantiation", RTH_HasInstantiation },
        { "System.RuntimeTypeHandle::IsGenericTypeDefinition", RTH_IsGenericTypeDefinition },
        { "System.RuntimeTypeHandle::IsGenericVariable", RTH_false },
        { "System.RuntimeTypeHandle::IsComObject", RTH_false },
        { "System.RuntimeTypeHandle::GetGenericParameterInfo", RTH_GetGenericParameterInfo },
        { "System.RuntimeTypeHandle::GetGenericTypeDefinition_impl", RTH_GetGenericTypeDefinition_impl },
        { "System.RuntimeTypeHandle::IsArray", RTH_IsArray },
        { "System.RuntimeTypeHandle::IsByRef", RTH_IsByRef },
        { "System.RuntimeTypeHandle::IsPointer", RTH_IsPointer },
        { "System.RuntimeTypeHandle::IsPrimitive", RTH_IsPrimitive },
        { "System.RuntimeTypeHandle::HasReferences", RTH_HasReferences },
        { "System.RuntimeTypeHandle::IsInstanceOfType", RTH_IsInstanceOfType },
        { "System.RuntimeTypeHandle::type_is_assignable_from", RTH_type_is_assignable_from },
        { "System.RuntimeTypeHandle::GetCorElementType", RTH_GetCorElementType },
        { "System.RuntimeTypeHandle::GetAssembly", RTH_GetAssembly },
        { "System.RuntimeTypeHandle::GetModule", RTH_GetModule },
        { "System.Reflection.MethodBase::GetMethodFromHandleInternalType_native", MethodBase_GetMethodFromHandleInternalType_native },
        { "System.Reflection.PropertyInfo::internal_from_handle_type", PropertyInfo_internal_from_handle_type },
        { "System.Reflection.EventInfo::internal_from_handle_type", EventInfo_internal_from_handle_type },
        { "System.Reflection.MonoMethodInfo::get_method_attributes", MonoMethodInfo_get_method_attributes },
        { "System.Reflection.MonoMethod::get_name", MonoMethod_get_name4 },
        { "System.Reflection.MonoMethod::GetGenericMethodDefinition_impl", MonoMethod_self },
        { "System.Reflection.MonoMethod::MakeGenericMethod_impl", MonoMethod_MakeGenericMethod_impl },
        { "System.Reflection.MonoMethod::GetPInvoke", MonoMethod_GetPInvoke },
        { "System.Reflection.MonoMethod::get_base_method", MonoMethod_get_base_method },
        { "System.Reflection.MonoMethod::get_core_clr_security_level", RT_security_level },
        { "System.Reflection.MonoCMethod::get_core_clr_security_level", RT_security_level },
        { "System.Reflection.MonoField::get_core_clr_security_level", RT_security_level },
        { "System.Reflection.MonoField::ResolveType", MonoField_ResolveType },
        { "System.Reflection.RtFieldInfo::UnsafeGetValue", MonoField_GetValueInternal },
        { "System.RuntimeFieldHandle::SetValueInternal", MonoField_SetValueInternal },
        { "System.Reflection.FieldInfo::GetTypeModifiers", FieldInfo_GetTypeModifiers },
        { "System.Reflection.FieldInfo::get_marshal_info", FieldInfo_get_marshal_info },
        { "System.Reflection.ParameterInfo::GetMetadataToken", ParameterInfo_GetMetadataToken },
        { "System.Reflection.ParameterInfo::GetTypeModifiers", ParameterInfo_GetTypeModifiers },
        { "System.Reflection.Module::get_MetadataToken", Module_get_MetadataToken },
        { "System.MonoCustomAttrs::GetCustomAttributesDataInternal", MonoCustomAttrs_GetCustomAttributesDataInternal },
        { "System.Reflection.MonoPropertyInfo::get_property_info", MonoPropertyInfo_get_property_info4 },
        { "System.Reflection.MonoPropertyInfo::get_default_value", MonoPropertyInfo_get_default_value },
        { "System.Reflection.AssemblyName::GetNativeName", AssemblyName_GetNativeName },
        { "System.Reflection.AssemblyName::ParseAssemblyName", AssemblyName_ParseAssemblyName },
        { "System.Reflection.AssemblyName::get_public_token", AssemblyName_get_public_token },
        { "Mono.RuntimeMarshal::FreeAssemblyName", RuntimeMarshal_FreeAssemblyName },
        { "System.Reflection.Assembly::GetManifestModuleInternal", Assembly_GetManifestModuleInternal },
        { "System.Reflection.Assembly::GetModulesInternal", Assembly_GetModulesInternal },
        { "System.Reflection.Assembly::GetFilesInternal", Assembly_GetFilesInternal },
        { "System.Reflection.Assembly::GetAotId", Assembly_GetAotId },
        { "System.Reflection.Assembly::InternalGetReferencedAssemblies", Assembly_InternalGetReferencedAssemblies },
        { "System.Reflection.Assembly::GetManifestResourceInfoInternal", Assembly_GetManifestResourceInfoInternal },
        { "System.Reflection.Assembly::load_with_partial_name", Assembly_load_with_partial_name },
        { "System.Reflection.Assembly::LoadPermissions", Assembly_LoadPermissions },
        { "System.Reflection.Assembly::get_global_assembly_cache", Assembly_get_global_assembly_cache },
        { "System.AppDomain::LoadAssemblyRaw", AppDomain_LoadAssemblyRaw },
        { "System.MissingMemberException::FormatSignature", MissingMemberException_FormatSignature },
        { "Mono.RuntimeClassHandle::GetTypeFromClass", RuntimeClassHandle_GetTypeFromClass },
    };
    nr_register_icalls(t, (int)(sizeof t / sizeof t[0]));
}
#endif

void nr_register_reflect_icalls(void)
{
    InitializeCriticalSection(&cache_lock);
    k_monomethod = nr_class_by_name("System.Reflection", "MonoMethod");
    k_monocmethod = nr_class_by_name("System.Reflection", "MonoCMethod");
    k_monofield = nr_class_by_name("System.Reflection", "MonoField");
    k_monoproperty = nr_class_by_name("System.Reflection", "MonoProperty");
    k_assembly = nr_class_by_name("System.Reflection", "Assembly");
    k_paraminfo = nr_class_by_name("System.Reflection", "ParameterInfo");
    k_module = nr_class_by_name("System.Reflection", "Module");
    static const NrIcall t[] = {
        { "System.Type::internal_from_handle", Type_internal_from_handle },
        { "System.Type::internal_from_name", Type_internal_from_name },
        { "System.Type::EqualsInternal", Type_EqualsInternal },
        { "System.Type::IsArrayImpl", Type_IsArrayImpl },
        { "System.Type::IsInstanceOfType", Type_IsInstanceOfType },
        { "System.Type::type_is_assignable_from", Type_type_is_assignable_from },
        { "System.Type::type_is_subtype_of", Type_type_is_subtype_of },
        { "System.Type::get_IsGenericType", Type_get_IsGenericType },
        { "System.Type::get_IsGenericTypeDefinition", Type_get_IsGenericTypeDefinition },
        { "System.Type::GetGenericTypeDefinition_impl", Type_GetGenericTypeDefinition_impl },
        { "System.Type::MakeGenericType", Type_MakeGenericType },
        { "System.Type::GetTypeCodeInternal", Type_GetTypeCodeInternal },
        { "System.Type::make_array_type", Type_make_array_type },
        { "System.Type::make_byref_type", Type_make_byref_type },
        { "System.Type::MakePointerType", Type_MakePointerType },
        { "System.Type::GetPacking", Type_GetPacking },
        { "System.MonoType::get_Name", MonoType_get_Name },
        { "System.MonoType::get_Namespace", MonoType_get_Namespace },
        { "System.MonoType::getFullName", MonoType_getFullName },
        { "System.MonoType::get_BaseType", MonoType_get_BaseType },
        { "System.MonoType::GetElementType", MonoType_GetElementType },
        { "System.MonoType::GetArrayRank", MonoType_GetArrayRank },
        { "System.MonoType::get_DeclaringType", MonoType_get_DeclaringType },
        { "System.MonoType::get_attributes", MonoType_get_attributes },
        { "System.MonoType::IsByRefImpl", MonoType_IsByRefImpl },
        { "System.MonoType::IsPointerImpl", MonoType_IsPointerImpl },
        { "System.MonoType::IsCOMObjectImpl", MonoType_IsCOMObjectImpl },
        { "System.MonoType::IsPrimitiveImpl", MonoType_IsPrimitiveImpl },
        { "System.MonoType::get_IsGenericParameter", MonoType_false },
        { "System.MonoType::get_DeclaringMethod", MonoType_null },
        { "System.MonoType::GetGenericArguments", MonoType_GetGenericArguments },
        { "System.MonoType::GetInterfaces", MonoType_GetInterfaces },
        { "System.MonoType::type_from_obj", MonoType_type_from_obj },
        { "System.MonoType::get_Assembly", MonoType_get_Assembly },
        { "System.MonoType::get_Module", MonoType_get_Module },
        { "System.MonoType::GetMethodsByName", MonoType_GetMethodsByName },
        { "System.MonoType::GetConstructors_internal", MonoType_GetConstructors_internal },
        { "System.MonoType::GetFields_internal", MonoType_GetFields_internal },
        { "System.MonoType::GetField", MonoType_GetField },
        { "System.MonoType::GetPropertiesByName", MonoType_GetPropertiesByName },
        { "System.MonoType::GetEvents_internal", MonoType_GetEvents_internal },
        { "System.MonoType::InternalGetEvent", MonoType_InternalGetEvent },
        { "System.MonoType::GetNestedTypes", MonoType_GetNestedTypes },
        { "System.MonoType::GetNestedType", MonoType_GetNestedType },
        { "System.MonoType::GetCorrespondingInflatedMethod", MonoType_GetCorrespondingInflatedMethod },
        { "System.MonoType::GetCorrespondingInflatedConstructor", MonoType_GetCorrespondingInflatedMethod },
        { "System.Enum::get_value", Enum_get_value },
        { "System.Enum::get_underlying_type", Enum_get_underlying_type },
        { "System.Enum::compare_value_to", Enum_compare_value_to },
        { "System.Enum::get_hashcode", Enum_get_hashcode },
        { "System.Enum::ToObject", Enum_ToObject },
        { "System.MonoEnumInfo::get_enum_info", Enum_get_enum_info },
        { "System.Activator::CreateInstanceInternal", Activator_CreateInstanceInternal },
        { "System.Reflection.MonoMethodInfo::get_method_info", MonoMethodInfo_get_method_info },
        { "System.Reflection.MonoMethodInfo::get_parameter_info", MonoMethodInfo_get_parameter_info },
        { "System.Reflection.MonoMethodInfo::get_retval_marshal", MonoMethodInfo_get_retval_marshal },
        { "System.Reflection.MonoMethod::get_name", MonoMethod_get_name },
        { "System.Reflection.MonoMethod::get_base_definition", MonoMethod_get_base_definition },
        { "System.Reflection.MonoMethod::get_IsGenericMethod", MonoMethod_get_IsGenericMethod },
        { "System.Reflection.MonoMethod::get_IsGenericMethodDefinition", MonoMethod_get_IsGenericMethodDefinition },
        { "System.Reflection.MonoMethod::MakeGenericMethod_impl", MonoMethod_MakeGenericMethod_impl },
        { "System.Reflection.MonoMethod::GetGenericMethodDefinition_impl", MonoMethod_GetGenericMethodDefinition_impl },
        { "System.Reflection.MonoMethod::GetGenericArguments", MonoMethod_GetGenericArguments },
        { "System.Reflection.MonoMethod::GetDllImportAttribute", MonoMethod_GetDllImportAttribute },
        { "System.Reflection.MonoMethod::InternalInvoke", MonoMethod_InternalInvoke },
        { "System.Reflection.MonoCMethod::InternalInvoke", MonoCMethod_InternalInvoke },
        { "System.Reflection.MethodBase::GetMethodFromHandleInternalType", MethodBase_GetMethodFromHandleInternalType },
        { "System.Reflection.MethodBase::GetCurrentMethod", MethodBase_GetCurrentMethod },
        { "System.Reflection.MonoField::GetValueInternal", MonoField_GetValueInternal },
        { "System.Reflection.MonoField::SetValueInternal", MonoField_SetValueInternal },
        { "System.Reflection.MonoField::GetParentType", MonoField_GetParentType },
        { "System.Reflection.MonoField::GetFieldOffset", MonoField_GetFieldOffset },
        { "System.Reflection.MonoField::GetRawConstantValue", MonoField_GetRawConstantValue },
        { "System.Reflection.FieldInfo::internal_from_handle_type", FieldInfo_internal_from_handle_type },
        { "System.Reflection.MonoPropertyInfo::get_property_info", MonoPropertyInfo_get_property_info },
        { "System.Reflection.MonoPropertyInfo::GetTypeModifiers", MonoPropertyInfo_GetTypeModifiers },
        { "System.MonoCustomAttrs::GetCustomAttributesInternal", MonoCustomAttrs_GetCustomAttributesInternal },
        { "System.MonoCustomAttrs::IsDefinedInternal", MonoCustomAttrs_IsDefinedInternal },
        { "System.Reflection.Assembly::GetExecutingAssembly", Assembly_GetExecutingAssembly },
        { "System.Reflection.Assembly::GetCallingAssembly", Assembly_GetExecutingAssembly },
        { "System.Reflection.Assembly::GetEntryAssembly", Assembly_GetExecutingAssembly },
        { "System.Reflection.Assembly::get_fullname", Assembly_get_fullname },
        { "System.Reflection.Assembly::get_location", Assembly_get_location },
        { "System.Reflection.Assembly::get_code_base", Assembly_get_code_base },
        { "System.Reflection.Assembly::GetTypes", Assembly_GetTypes },
        { "System.Reflection.Assembly::InternalGetType", Assembly_InternalGetType },
        { "System.Reflection.Assembly::get_ReflectionOnly", Assembly_get_ReflectionOnly },
        { "System.Reflection.Assembly::get_EntryPoint", Assembly_null1 },
        { "System.Reflection.Assembly::GetManifestResourceNames", Assembly_GetManifestResourceNames },
        { "System.Reflection.Assembly::GetManifestResourceInternal", Assembly_GetManifestResourceInternal },
        { "System.Reflection.Assembly::FillName", Assembly_FillName },
        { "System.Reflection.Assembly::GetReferencedAssemblies", Assembly_GetReferencedAssemblies },
        { "System.Reflection.Assembly::InternalImageRuntimeVersion", Assembly_InternalImageRuntimeVersion },
        { "System.Reflection.Assembly::LoadFrom", Assembly_load },
        { "System.AppDomain::LoadAssembly", AppDomain_LoadAssembly },
        { "System.AppDomain::GetAssemblies", AppDomain_GetAssemblies },
        { "System.Reflection.MemberInfo::get_MetadataToken", MemberInfo_get_MetadataToken },
        { "System.Diagnostics.StackTrace::get_trace", StackTrace_get_trace },
        { "System.Diagnostics.StackFrame::get_frame_info", StackFrame_get_frame_info },
        { "System.Delegate::CreateDelegate_internal", Delegate_CreateDelegate_internal },
        { "System.Delegate::SetMulticastInvoke", Delegate_SetMulticastInvoke },
    };
    nr_register_icalls(t, (int)(sizeof t / sizeof t[0]));
#if NR_CORLIB4
    register_reflect4();
#endif
}
