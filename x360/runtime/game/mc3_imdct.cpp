// MC3 (2026-11-19): the WMA decoder's inverse MDCT (0x42F5CC, [decoder+0x3FC]), its rotations native.
//
// Console log 2026-11-19 04:42 (garage): one WMA block cost ~1.6 ms on the console against ~0.45 ms in Xenia, and this
// function was 45% of it (1105 ms of 2482 in the 10 s of a song change, fps 4.6). It is x87 code: two rotation loops
// before the FFT and one after, each reading and writing the block's floats one by one. In the translated code a guest
// float goes through a byte reversed integer load and a store/reload to reach the FPU (X86LeF32), a load-hit-store on
// the Xenon for each of the ~9 float accesses of every iteration.
//
// Here the block (n floats) is read into a host buffer once, the rotations run on it with the same operations in the
// same order (x87 values are doubles in the translated code too, memory values floats), and the FFT stays the game's:
// [decoder+0x400] (0x42F3D0, the SSE one, translated to VMX) on the guest buffer, so the block goes back to the guest
// before it and comes in again after. Block sizes outside the game's table (n < 64 or n > 2048, the fsin/fcos path) run
// the game's code.
// game:\wmacheck.txt: each block runs the game's code first, then this on the same input; the outputs are compared and
// the game's kept ("wma imdct: N blocks checked ...").
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xtl.h>

#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "host.h"

using kernel::R32;
using kernel::W32;
using kernel::W16;

extern int64_t g_mc3_wma_ns[10];
extern uint32_t g_mc3_wma_calls[10];

namespace {
const uint32_t kTables = 0x00439400;  // per n>>7: pointer to the rotation constants
const bool kCheck = GetFileAttributesA("game:\\wmacheck.txt") != DWORD(-1);
uint32_t g_checked = 0, g_exact = 0, g_close = 0, g_far = 0, g_native = 0, g_game = 0;
double g_worst = 0;

__declspec(align(16)) union {
    uint32_t u[2048];
    float f[2048];
} g_h;
uint32_t g_save[2048], g_ref[2048];

inline float F(uint32_t bits) {
    union { uint32_t u; float f; } c;
    c.u = bits;
    return c.f;
}
inline uint32_t U(float f) {
    union { uint32_t u; float f; } c;
    c.f = f;
    return c.u;
}

void In(uint32_t buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) g_h.u[i] = R32(buf + 4 * i);
}
void Out(uint32_t buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) W32(buf + 4 * i, g_h.u[i]);
}

// the game's 0x42F5CC for 64 <= n <= 2048, its x87 stack kept as doubles
void Imdct(X86Context& ctx, uint32_t obj, uint32_t buf, float scale, uint32_t n, uint32_t scratch_esp) {
    float* h = g_h.f;
    const uint32_t t = R32(kTables + 4 * (n >> 7));
    const float t0 = F(R32(t)), t1 = F(R32(t + 4)), t2 = F(R32(t + 8)), t3 = F(R32(t + 12));
    const float t4 = F(R32(t + 0x10)), t5 = F(R32(t + 0x14)), t10 = F(R32(t + 0x28));
    const double sc = scale;
    double A = sc * t1;
    float wr = float(-(sc * t0));
    float wi = float(sc * t3);
    double B = sc * t2;
    const float s = -t10;
    const uint32_t quarter = n >> 2;

    In(buf, n);
    // first rotation: the front half against the back, the back's odd slots taking the front's odd values
    uint32_t p = 0, q = n - 1;
    for (uint32_t i = 0; i < quarter; i++, p += 2, q -= 2) {
        const double X = h[q];
        g_h.u[q] = g_h.u[p + 1];
        const double P = h[p];
        h[p] = float(P * A - X * double(wr));
        h[p + 1] = float(P * double(wr) + X * A);
        const float t8 = float(double(wi) - double(s) * double(wr));
        const float tn = float(double(s) * A + B);
        wi = float(A);
        B = double(wr);
        A = double(t8);
        wr = tn;
    }
    // second rotation: the back half in place
    for (uint32_t i = 0; i < quarter; i++, p += 2) {
        const double P = h[p], Q = h[p + 1];
        h[p] = float(P * A - Q * double(wr));
        h[p + 1] = float(Q * A + P * double(wr));
        const float t8 = float(double(wi) - double(s) * double(wr));
        const float tn = float(double(s) * A + B);
        wi = float(A);
        B = double(wr);
        A = double(t8);
        wr = tn;
    }
    Out(buf, n);

    // the game's FFT on the guest buffer: fft(buf, log2(n) - 1), stdcall
    uint32_t lg = 0;
    while ((n >> lg) > 1) lg++;
    ctx.esp = scratch_esp;
    kernel::CallGuest(ctx, R32(obj + 0x400), buf, lg - 1);

    In(buf, n);
    // post rotation: front pairs against back pairs
    double C = 1.0;
    float w = 0.0f, m18 = t5, m1C = t4;
    p = 0;
    q = n - 2;
    for (uint32_t i = 0; i < quarter; i++, p += 2, q -= 2) {
        const double P0 = h[p], P1 = h[p + 1];
        const float q0 = h[q], q1 = h[q + 1];
        h[p] = float(P0 * C - P1 * double(w));
        h[q + 1] = float(-double(w) * P0 - P1 * C);
        const float u = float(double(m18) - double(s) * double(w));
        const float v = float(double(s) * C + double(m1C));
        m1C = w;
        m18 = float(C);
        w = v;
        C = double(u);
        h[p + 1] = float(double(q0) * double(u) + double(v) * double(q1));
        h[q] = float(-double(v) * double(q0) + double(u) * double(q1));
    }
    Out(buf, n);
}
}  // namespace

// MC3 (2026-11-19): the window and 16-bit output (0x42B5C2(decoder, channel, unused), ret 0xC). Console log 05:50 with
// the IMDCT native: 0.248 ms a call, now the decoder's largest part. Per sample: window * data (the second half with the
// window negated), rounded to float, fistp (the x87 rounding mode), clamped to -65536..65535, the low 16 bits stored
// (stride: the channel count), and a register of the samples' sign bits stored every 16 samples. Each sample took two
// float loads, a float store and reload, and the fistp's store and reload: load-hit-stores. Here the floats come in
// once, pass A rounds every sample to a double whose low word is the integer (the 1.5 * 2^52 sum: round to nearest
// even, as fistp with the default control word), pass B reads those words long after their stores and writes the
// samples. The game's code runs when a half is not a multiple of 16 samples (its rest loop) or the rounding mode is
// not to nearest.
namespace {
const int kMaxOut = 4096;
uint32_t g_w[kMaxOut], g_d[kMaxOut];
union Magic {
    double d;
    int32_t i[2];  // big endian: i[1] is the low word
} g_t[kMaxOut];
uint32_t g_out_native = 0, g_out_game = 0, g_out_checked = 0, g_out_differ = 0;
uint8_t g_out_save[kMaxOut * 2 * 8 + kMaxOut / 8], g_out_ref[kMaxOut * 2 * 8 + kMaxOut / 8];

// returns false when the game's code must run
bool Output(X86Context& ctx, uint32_t obj, uint32_t ch) {
    if (((ctx.fcw >> 10) & 3) != 0) return false;
    const uint32_t nch = kernel::R16(obj + 0x58);
    const int32_t limit = int32_t(R32(obj + 0x398));
    const uint32_t esi0 = R32(obj + 0x42C);
    const int32_t pos0 = int16_t(kernel::R16(ch + 0x38));
    const uint32_t edi0 = R32(ch + 0x3C), out0 = R32(ch + 0x40), mask0 = R32(ch + 0x44);
    int32_t c1 = int32_t(R32(obj + 0xA0)) * 3 - pos0;
    if (c1 < 0) c1 = 0;
    if (c1 & 15) return false;
    const int32_t n1 = c1;
    int32_t c2 = limit - (pos0 + n1);
    if (c2 < 0) c2 = 0;
    if (c2 & 15) return false;
    const int32_t n2 = c2;
    if (n1 + n2 > kMaxOut || nch == 0 || nch > 8) return false;
    const int32_t n = n1 + n2;
    // in: the window backwards from esi0 through both halves; the data backwards from edi0, then forwards again
    for (int32_t k = 0; k < n; k++) g_w[k] = R32(esi0 - 4 * uint32_t(k + 1));
    for (int32_t k = 0; k < n1; k++) g_d[k] = R32(edi0 - 4 * uint32_t(k + 1));
    const uint32_t mid = edi0 - 4 * uint32_t(n1);
    for (int32_t k = 0; k < n2; k++) g_d[n1 + k] = R32(mid + 4 * uint32_t(k));
    // pass A
    for (int32_t k = 0; k < n; k++) {
        const double w = k < n1 ? double(F(g_w[k])) : -double(F(g_w[k]));
        const float pf = float(w * double(F(g_d[k])));
        double v;
        if (!(pf >= -65536.0f)) v = -65536.0;  // NaN too: fistp's integer indefinite, clamped
        else if (pf > 65535.0f) v = 65535.0;
        else v = double(pf);
        g_t[k].d = v + 6755399441055744.0;
    }
    // pass B
    uint32_t out = out0, mask = mask0, reg = 0;
    for (int32_t k = 0; k < n; k++) {
        const int32_t s = g_t[k].i[1];
        W16(out, uint16_t(s));
        out += 2 * nch;
        reg = (reg << 1) | (uint32_t(s) >> 31);
        if ((k & 15) == 15) {
            W16(mask, uint16_t(reg));
            mask += 2;
        }
    }
    W16(ch + 0x38, uint16_t(pos0 + n1 + (n2 >> 4)));  // the game adds the second half's group count, not its samples
    return true;
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_0042B5C2);
X86_FUNC(sub_0042B5C2) {
    const int64_t t0 = host::NowNs();
    const uint32_t esp0 = ctx.esp;
    const uint32_t obj = R32(esp0 + 4), ch = R32(esp0 + 8);
    // the spans the game's code writes, for the check
    const uint32_t nch = kernel::R16(obj + 0x58);
    const uint32_t out0 = R32(ch + 0x40), mask0 = R32(ch + 0x44);
    // the samples this call writes (both halves), as Output counts them
    int32_t c1 = int32_t(R32(obj + 0xA0)) * 3 - int16_t(kernel::R16(ch + 0x38));
    if (c1 < 0) c1 = 0;
    int32_t c2 = int32_t(R32(obj + 0x398)) - (int16_t(kernel::R16(ch + 0x38)) + c1);
    if (c2 < 0) c2 = 0;
    const bool fits = c1 + c2 <= kMaxOut && nch && nch <= 8;
    const uint32_t span = fits ? uint32_t(c1 + c2) * 2 * nch : 0, mspan = fits ? uint32_t(c1 + c2) / 8 : 0;
    const bool check = kCheck && fits;
    uint16_t pos_ref = 0;
    if (check) {
        for (uint32_t i = 0; i < span; i++) g_out_save[i] = kernel::G8(out0 + i);
        for (uint32_t i = 0; i < mspan; i++) g_out_save[span + i] = kernel::G8(mask0 + i);
        const uint16_t pos_in = kernel::R16(ch + 0x38);
        __imp__sub_0042B5C2(ctx, base);
        for (uint32_t i = 0; i < span; i++) {
            g_out_ref[i] = kernel::G8(out0 + i);
            kernel::G8(out0 + i) = g_out_save[i];
        }
        for (uint32_t i = 0; i < mspan; i++) {
            g_out_ref[span + i] = kernel::G8(mask0 + i);
            kernel::G8(mask0 + i) = g_out_save[span + i];
        }
        pos_ref = kernel::R16(ch + 0x38);
        W16(ch + 0x38, pos_in);
        ctx.esp = esp0;
    }
    if (Output(ctx, obj, ch)) {
        g_out_native++;
        ctx.eax = 0;
        ctx.esp = esp0 + 16;
        if (check) {
            uint32_t bad = 0;
            for (uint32_t i = 0; i < span + mspan; i++) {
                const uint8_t mine = i < span ? kernel::G8(out0 + i) : kernel::G8(mask0 + i - span);
                if (mine != g_out_ref[i]) bad++;
            }
            if (kernel::R16(ch + 0x38) != pos_ref) bad++;
            g_out_checked++;
            if (bad) {
                g_out_differ++;
                if (g_out_differ <= 8) LOGW("wma output: call %u differs in %u bytes (pos %u/%u)", g_out_checked, bad, kernel::R16(ch + 0x38), pos_ref);
                for (uint32_t i = 0; i < span; i++) kernel::G8(out0 + i) = g_out_ref[i];
                for (uint32_t i = 0; i < mspan; i++) kernel::G8(mask0 + i) = g_out_ref[span + i];
                W16(ch + 0x38, pos_ref);
            }
            if ((g_out_checked & 0x3FF) == 0) LOGI("wma output: %u calls checked, %u differ", g_out_checked, g_out_differ);
        }
    } else {
        g_out_game++;
        if (check) {
            // the game's result is already in place: put it back as it was after its run
            for (uint32_t i = 0; i < span; i++) kernel::G8(out0 + i) = g_out_ref[i];
            for (uint32_t i = 0; i < mspan; i++) kernel::G8(mask0 + i) = g_out_ref[span + i];
            W16(ch + 0x38, pos_ref);
            ctx.esp = esp0 + 16;
            ctx.eax = 0;
        } else {
            __imp__sub_0042B5C2(ctx, base);
        }
        if (g_out_game <= 4) LOGI("wma output: the game's code (call %u)", g_out_native + g_out_game);
    }
    g_mc3_wma_ns[7] += host::NowNs() - t0;
    g_mc3_wma_calls[7]++;
}

X86_EXTERN_FUNC(__imp__sub_0042F5CC);
X86_FUNC(sub_0042F5CC) {
    const int64_t t0 = host::NowNs();
    const uint32_t esp0 = ctx.esp;
    const uint32_t obj = R32(esp0 + 4), buf = R32(esp0 + 8), sbits = R32(esp0 + 12), outp = R32(esp0 + 16);
    const uint32_t n = R32(obj + 0x60);
    if (n < 64 || n > 2048 || (n & (n - 1))) {
        g_game++;
        __imp__sub_0042F5CC(ctx, base);
    } else {
        g_native++;
        const uint32_t ebx = ctx.ebx, esi = ctx.esi, edi = ctx.edi, ebp = ctx.ebp;
        if (kCheck) {
            for (uint32_t i = 0; i < n; i++) g_save[i] = R32(buf + 4 * i);
            __imp__sub_0042F5CC(ctx, base);
            for (uint32_t i = 0; i < n; i++) {
                g_ref[i] = R32(buf + 4 * i);
                W32(buf + 4 * i, g_save[i]);
            }
            ctx.esp = esp0;
        }
        Imdct(ctx, obj, buf, F(sbits), n, esp0 - 16);
        if (kCheck) {
            uint32_t exact = 0;
            double worst = 0;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t mine = R32(buf + 4 * i);
                if (mine == g_ref[i]) {
                    exact++;
                    continue;
                }
                const double a = F(mine), b = F(g_ref[i]);
                const double d = fabs(a - b) / (fabs(b) > 1.0 ? fabs(b) : 1.0);
                if (d > worst) worst = d;
                W32(buf + 4 * i, g_ref[i]);
            }
            g_checked++;
            if (exact == n) g_exact++;
            else if (worst < 1e-4) g_close++;
            else {
                g_far++;
                if (g_far <= 8) LOGW("wma imdct: block %u (n %u) differs: %u of %u exact, worst %.3g", g_checked, n, exact, n, worst);
            }
            if (worst > g_worst) g_worst = worst;
            if ((g_checked & 0x3FF) == 0)
                LOGI("wma imdct: %u blocks checked: %u exact, %u close, %u far (worst %.3g)", g_checked, g_exact, g_close, g_far,
                     g_worst);
        }
        if (outp) W32(outp, 0);
        ctx.eax = 0;
        ctx.ebx = ebx;
        ctx.esi = esi;
        ctx.edi = edi;
        ctx.ebp = ebp;
        ctx.esp = esp0 + 20;
    }
    g_mc3_wma_ns[6] += host::NowNs() - t0;
    g_mc3_wma_calls[6]++;
    if (g_native + g_game == 1)
        LOGI("wma imdct: native rotations, the FFT at %08X (n %u)", R32(obj + 0x400), n);
}
