// Xbox 360 build of the PC port's runtime/xdk/crt_math.cpp (MC3, 2026-11-11).
// The CRT's x87 math checks its argument in a helper call and branches on the ZF that call returns. Recompiled code
// does not carry flags across call and ret, so the functions read stale flags and returned NaN at random. Computed on
// the host instead (also far cheaper than the recompiled x87 series).
#include <math.h>

#include "xdk/hle.h"

// each function's body, shared by its stack entry (sin) and its register entry (_CIsin): x in st0, popped by its ret
#define CRT_MATH(name, fn)         \
    X86_FUNC(GUEST(name)) {        \
        (void)base;                \
        X87_ST(0) = fn(X87_ST(0)); \
        ctx.esp += 4;              \
    }
#if HAS_crt_sin
CRT_MATH(crt_sin, sin)
#endif
#if HAS_crt_cos
CRT_MATH(crt_cos, cos)
#endif
#if HAS_crt_tan
CRT_MATH(crt_tan, tan)
#endif
#if HAS_crt_asin
CRT_MATH(crt_asin, asin)
#endif
#if HAS_crt_acos
CRT_MATH(crt_acos, acos)
#endif
#if HAS_crt_atan
CRT_MATH(crt_atan, atan)
#endif
#if HAS_crt_sqrt
CRT_MATH(crt_sqrt, sqrt)
#endif
#if HAS_crt_log
CRT_MATH(crt_log, log)
#endif
#if HAS_crt_log10
CRT_MATH(crt_log10, log10)
#endif
#undef CRT_MATH

#if HAS_crt_pow
// x and y above the return address, y in st0
X86_FUNC(GUEST(crt_pow)) {
    X87_ST(0) = pow(double(X86_MEMF64(ctx.esp + 4)), double(X86_MEMF64(ctx.esp + 12)));
    ctx.esp += 4;
}
#endif

// configs written before the bodies had names
#if HAS__CIacos
X86_FUNC(GUEST(_CIacos)) {
    (void)base;
    X87_ST(0) = acos(X87_ST(0));
    ctx.esp += 4;
}
#endif

#if HAS__CIasin
X86_FUNC(GUEST(_CIasin)) {
    (void)base;
    X87_ST(0) = asin(X87_ST(0));
    ctx.esp += 4;
}
#endif
