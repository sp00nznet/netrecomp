/* nr_decimal.c: System.Decimal internal calls on OLE Automation's VarDec* (oleaut32).
 * mono's Decimal is { flags (scale in bits 16-23, sign bit 31), hi, lo, mid }.
 * Return codes follow mono: 0 ok, 2 overflow, 4 divide by zero. */
#include "nrrt.h"
#include <oleauto.h>

typedef struct MDec { uint32_t flags, hi, lo, mid; } MDec;

static DECIMAL to_ole(const MDec* m)
{
    DECIMAL d; memset(&d, 0, sizeof d);
    d.scale = (BYTE)(m->flags >> 16);
    d.sign = (m->flags & 0x80000000u) ? DECIMAL_NEG : 0;
    d.Hi32 = m->hi; d.Lo32 = m->lo; d.Mid32 = m->mid;
    return d;
}
static void from_ole(MDec* m, const DECIMAL* d)
{
    m->flags = ((uint32_t)d->scale << 16) | ((d->sign & DECIMAL_NEG) ? 0x80000000u : 0);
    m->hi = d->Hi32; m->lo = d->Lo32; m->mid = d->Mid32;
    if (!m->hi && !m->lo && !m->mid) m->flags = 0;
}
static int32_t rc(HRESULT h) { return h == S_OK ? 0 : h == DISP_E_DIVBYZERO ? 4 : 2; }

/* digits only; decPos = how many of them are before the decimal point */
static int32_t string2decimal(MDec* v, void* s, uint32_t decPos, int32_t sign)
{
    NrString* str = (NrString*)s;
    int n = str ? str->length : 0;
    WCHAR buf[128]; int j = 0;
    if (sign) buf[j++] = L'-';
    for (int i = 0; i < n && j < 120; i++) { if ((uint32_t)i == decPos) buf[j++] = L'.'; buf[j++] = str->chars[i]; }
    if (j == 0 || (j == 1 && sign)) buf[j++] = L'0';
    buf[j] = 0;
    DECIMAL d;
    HRESULT h = VarDecFromStr(buf, LOCALE_INVARIANT, 0, &d);
    if (h != S_OK) return rc(h);
    from_ole(v, &d);
    return 0;
}
static DECIMAL pow10(int e) { DECIMAL d; VarDecFromI4(1, &d); DECIMAL ten; VarDecFromI4(10, &ten); while (e-- > 0) VarDecMul(&d, &ten, &d); return d; }
static int32_t decimalSetExponent(MDec* v, int32_t exp)
{
    /* a positive exponent first eats the scale (1.5e3 is 1500, not 1500.0) */
    int k = exp > 0 ? (int)((v->flags >> 16) & 0xff) : 0;
    if (k > exp) k = exp;
    v->flags -= (uint32_t)k << 16; exp -= k;
    DECIMAL a = to_ole(v), r, p = pow10(exp < 0 ? -exp : exp);
    HRESULT h = exp >= 0 ? VarDecMul(&a, &p, &r) : VarDecDiv(&a, &p, &r);
    if (h == S_OK) from_ole(v, &r);
    return rc(h);
}
static double decimal2double(MDec* v) { DECIMAL a = to_ole(v); double r = 0; VarR8FromDec(&a, &r); return r; }
static int32_t decimalCompare(MDec* a, MDec* b)
{
    DECIMAL x = to_ole(a), y = to_ole(b);
    HRESULT h = VarDecCmp(&x, &y);
    return h == VARCMP_LT ? -1 : h == VARCMP_GT ? 1 : 0;
}
static void decimalFloorAndTrunc(MDec* v, int32_t floorFlag)
{
    DECIMAL a = to_ole(v), r;
    if ((floorFlag ? VarDecInt(&a, &r) : VarDecFix(&a, &r)) == S_OK) from_ole(v, &r);
}
static int32_t decimal2Int64(MDec* v, int64_t* out) { DECIMAL a = to_ole(v), t; VarDecFix(&a, &t); LONG64 r = 0; HRESULT h = VarI8FromDec(&t, &r); *out = r; return rc(h); }
static int32_t decimal2UInt64(MDec* v, uint64_t* out) { DECIMAL a = to_ole(v), t; VarDecFix(&a, &t); ULONG64 r = 0; HRESULT h = VarUI8FromDec(&t, &r); *out = r; return rc(h); }
static int32_t decimalIncr(MDec* a, MDec* b) { DECIMAL x = to_ole(a), y = to_ole(b), r; HRESULT h = VarDecAdd(&x, &y, &r); if (h == S_OK) from_ole(a, &r); return rc(h); }
static int32_t decimalMult(MDec* a, MDec* b) { DECIMAL x = to_ole(a), y = to_ole(b), r; HRESULT h = VarDecMul(&x, &y, &r); if (h == S_OK) from_ole(a, &r); return rc(h); }
static int32_t decimalDiv(MDec* c, MDec* a, MDec* b) { DECIMAL x = to_ole(a), y = to_ole(b), r; HRESULT h = VarDecDiv(&x, &y, &r); if (h == S_OK) from_ole(c, &r); return rc(h); }
static int32_t decimalIntDiv(MDec* c, MDec* a, MDec* b) { int32_t e = decimalDiv(c, a, b); if (!e) decimalFloorAndTrunc(c, 0); return e; }
static int32_t double2decimal(MDec* v, double d, int32_t digits) { (void)digits; DECIMAL r; HRESULT h = VarDecFromR8(d, &r); if (h == S_OK) from_ole(v, &r); return rc(h); }

void nr_register_decimal_icalls(void)
{
#define D(n) { "System.Decimal::" #n, (void*)n }
    static const NrIcall t[] = {
        D(string2decimal), D(decimalSetExponent), D(decimal2double), D(decimalCompare), D(decimalFloorAndTrunc),
        D(decimal2Int64), D(decimal2UInt64), D(decimalIncr), D(decimalMult), D(decimalDiv), D(decimalIntDiv), D(double2decimal),
    };
#undef D
    nr_register_icalls(t, (int)(sizeof t / sizeof t[0]));
}
