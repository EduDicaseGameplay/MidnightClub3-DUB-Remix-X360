// MC3 (2026-11-12): the title's hottest math routines, in VMX on the host.
//
// 0x8AB20 multiplies two 4x4 float matrices (cdecl: out, b, a; out = a * b, rows of four floats). On the console it
// is 3 to 4% of the title's thread in a race (mc3_360.log of 2026-11-12 05:54, return address 0x8B5E3), and the
// recompiled body does 64 scalar SSE loads, each an unaligned VMX load, a byte swap and a mask. Here: 8 row loads,
// 16 multiply-adds, 4 row stores. The sums are fused (vmaddfp), the last bits can differ from the x86's; when out
// overlaps an input, the recompiled body runs, so a product written over its own input reads what the x86 read.
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

extern "C" void __imp__sub_0008AB20(X86Context& ctx, uint8_t* base);

X86_FUNC(sub_0008AB20) {
    const uint32_t out = kernel::R32(ctx.esp + 4), b = kernel::R32(ctx.esp + 8), a = kernel::R32(ctx.esp + 12);
    if ((out < a + 64 && a < out + 64) || (out < b + 64 && b < out + 64)) {
        __imp__sub_0008AB20(ctx, base);
        return;
    }
    const __vector4 b0 = x86_xs_ldu(base, b), b1 = x86_xs_ldu(base, b + 16), b2 = x86_xs_ldu(base, b + 32),
                    b3 = x86_xs_ldu(base, b + 48);
    __vector4 r[4];
    for (int i = 0; i < 4; i++) {
        const __vector4 ar = x86_xs_ldu(base, a + i * 16);
        __vector4 s = __vmulfp(__vspltw(ar, 0), b0);
        s = __vmaddfp(__vspltw(ar, 1), b1, s);
        s = __vmaddfp(__vspltw(ar, 2), b2, s);
        r[i] = __vmaddfp(__vspltw(ar, 3), b3, s);
    }
    for (int i = 0; i < 4; i++) x86_xs_stu(base, out + i * 16, r[i]);
    // the registers the x86 body leaves: eax = b, ecx = a, edx = out (xmm0 and xmm1 are scratch to the callers)
    ctx.eax = b;
    ctx.ecx = a;
    ctx.edx = out;
    ctx.esp += 4;  // ret
}
