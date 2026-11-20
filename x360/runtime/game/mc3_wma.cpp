// MC3 (2026-11-18): the WMA music decoder's coefficient reading, native.
//
// XACT decodes the songs (AUDIO\XSTREAMS\mx.xwb, WMA) on the title's thread, in XACTEngineDoWork. Xenia, race, 10 s
// (game/mc3_xact.cpp timers): the songs' stream was 95% of the engine's work (300 to 500 ms), every other stream under
// 2 ms; inside the decoder, the coefficient reader 0x42D66A ran ~250 000 times and the bit reader under it ~270 000
// times. Console, heavy streets: the engine took up to 1.5 s of every 10 s of the title's thread. Each of these calls
// is a few dozen x86 instructions, but the translated code pays for the indirect calls, the flags and a byte reversed
// guest access per field. Here they are plain C++ on the same guest structures, instruction for instruction:
//   0x42B2D9 peek(bits, n, out): fill the 32-bit window from the byte stream, out = the next n bits at the top
//   0x42B3CC skip(bits, n)
//   0x430646 huffman(table, bits, len*, sym*, rest*): 23 bits peeked through bits->[0x134], a 2/2/1-bit tree walk
//   0x42D66A coefficient(state, tables, unused): one run/level pair (an escape goes to the game's 0x42B35D/0x42B4E4)
// The stream's own refill (0x42B06D, when the bytes run out) and the escape readers stay the game's code.
// game:\wmacheck.txt: every coefficient runs the game's code first, then this on the same state, and any difference
// is logged ("wma: differs") and the game's result kept: the check for a new build, not for playing.
#include <stdio.h>
#include <string.h>
#include <xtl.h>

#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "host.h"

using kernel::R16;
using kernel::R32;
using kernel::W16;
using kernel::W32;

namespace {
const uint32_t kErr = 0x80040004u;
const uint32_t kRefill = 0x0042B06D, kEscapeA = 0x0042B35D, kEscapeB = 0x0042B4E4;
const uint32_t kPeekFn = 0x0042B2D9;

// bit reader fields: +0x18 byte pointer, +0x1C bytes left, +0x20 window, +0x24 bits in the window
uint32_t Fill(X86Context& ctx, uint32_t s, uint32_t scratch) {
    uint32_t eax = 0;
    if (R32(s + 0x24) > 0x18) return 0;
    for (;;) {
        int32_t left = int32_t(R32(s + 0x1C));
        if (left <= 0) break;
        const uint32_t p = R32(s + 0x18);
        W32(s + 0x20, (R32(s + 0x20) << 8) | kernel::G8(p));
        W32(s + 0x24, R32(s + 0x24) + 8);
        W32(s + 0x18, p + 1);
        W32(s + 0x1C, uint32_t(left - 1));
        if (R32(s + 0x24) > 0x18) break;
    }
    if (R32(s + 0x24) <= 0x18 && int32_t(R32(s + 0x1C)) <= 0) {
        W32(scratch, s);
        eax = kernel::CallGuest(ctx, kRefill, s, scratch);
    }
    return eax;
}

uint32_t Peek(X86Context& ctx, uint32_t s, uint32_t n, uint32_t& out, uint32_t scratch) {
    uint32_t eax = Fill(ctx, s, scratch);
    if (int32_t(eax) < 0) return eax;
    const uint32_t cnt = R32(s + 0x24);
    if (cnt < n) return kErr;
    const uint32_t v = R32(s + 0x20) >> ((uint8_t(cnt) - uint8_t(n)) & 31);
    out = v << ((32 - n) & 31);
    return eax;
}

uint32_t Skip(X86Context& ctx, uint32_t s, uint32_t n, uint32_t scratch) {
    uint32_t eax = Fill(ctx, s, scratch);
    if (int32_t(eax) < 0) return eax;
    const uint32_t cnt = R32(s + 0x24);
    if (n > cnt) return kErr;
    W32(s + 0x24, cnt - n);
    return eax;
}

uint32_t Huffman(X86Context& ctx, uint32_t table, uint32_t bits, uint32_t& len, uint32_t& sym, uint32_t* rest,
                 uint32_t scratch) {
    uint32_t v = 0, eax;
    const uint32_t fn = R32(bits + 0x134);
    if (fn == kPeekFn) {
        eax = Peek(ctx, bits, 0x17, v, scratch);
    } else {
        eax = kernel::CallGuest(ctx, fn, bits, 0x17, scratch);
        v = R32(scratch);
    }
    if (int32_t(eax) < 0) return eax;
    uint32_t e = v, p = table, c = 0;
    bool found = false;
    for (int i = 0; i < 5 && !found; i++) {
        p += (e >> 30) * 2;
        c = R16(p);
        if (c & 0x8000) { found = true; break; }
        e <<= 2;
        p += c * 2;
    }
    for (int i = 0; i < 3 && !found; i++) {
        p += (e >> 30) * 2;
        c = R16(p);
        if (c & 0x8000) { found = true; break; }
        e <<= 2;
        p += c * 2;
    }
    for (int i = 0; i < 6 && !found; i++) {
        p += (e >> 31) * 2;
        c = R16(p);
        if (c & 0x8000) { found = true; break; }
        e <<= 1;
        p += c * 2;
    }
    len = (c >> 10) & 0x1F;
    sym = c & 0x3FF;
    if (sym >= 0x3FC) sym = R16(p + (c & 3) * 2 + 2);
    if (rest) *rest = v << (len & 31);
    return eax;
}

// 0x42D66A body; slot = the guest address of its first argument (written by the escape reader 0x42B4E4)
uint32_t Coefficient(X86Context& ctx, uint32_t state, uint32_t tables, uint32_t slot, uint32_t scratch) {
    const uint32_t st = R32(state + 0x20);
    const uint32_t o = R32(state);
    const uint32_t bits = state + 0x30;
    uint32_t eax = 0;
    uint32_t sym = 0, rest = 0;
    bool escape = false;
    if (st == 0) {
        uint32_t len = 0;
        eax = Huffman(ctx, R32(tables + 0x14), bits, len, sym, &rest, scratch);
        if (int32_t(eax) < 0) return eax;
        W32(slot, len);
        eax = Skip(ctx, bits, len, scratch);
        if (int32_t(eax) < 0) return eax;
        if (sym == 0) escape = true;
    } else if (st == 3) {
        escape = true;
    } else {
        return 0;
    }
    if (escape) {
        W32(state + 0x20, 3);
        const uint32_t ax = (uint32_t(uint16_t(R16(o + 0x314) + R16(o + 0x3C))) + 1) & 0xFFFF;
        eax = kernel::CallGuest(ctx, kEscapeA, bits, ax);
        if (int32_t(eax) < 0) return eax;
        eax = kernel::CallGuest(ctx, kEscapeB, bits, ax, slot);
        if (int32_t(eax) < 0) return eax;
        const uint32_t sv = R32(slot);
        const uint32_t edx = uint16_t(R16(R32(state) + 0x314) + 1);
        W16(o + 0x20, uint16_t(sv >> (edx & 31)));
        W32(o + 0x24, (sv & 1) - 1);
        int32_t a = int16_t(uint16_t(0xFFFFFFFFu >> ((0x20 - edx) & 31)));
        int32_t b = int16_t(uint16_t(sv));
        a >>= 1;
        b >>= 1;
        W32(o + 0x1C, uint32_t(a & b) & 0x7FFFFFFF);
        W32(state + 0x20, 0);
        return eax;
    }
    if (sym == 1) {
        W16(o + 0x20, 0);
        W32(o + 0x28, 0xFFFFFFFFu);
        W32(o + 0x2C, 0xFFFFFFFFu);
        W32(o + 0x1C, R32(tables + 0x20) - uint32_t(int32_t(int16_t(R16(o + 0x74)))) - 1);
        return eax;
    }
    eax = Skip(ctx, bits, 1, scratch);
    if (int32_t(eax) < 0) return eax;
    W32(o + 0x1C, uint32_t(R16(R32(tables + 0x18) + sym * 2 - 4)) & 0x7FFFFFFF);
    W16(o + 0x20, R16(R32(tables + 0x1C) + sym * 2 - 4));
    W32(o + 0x24, (rest >> 31) - 1);
    return eax;
}

const bool kCheck = GetFileAttributesA("game:\\wmacheck.txt") != DWORD(-1);
uint32_t g_checked = 0, g_differ = 0;

struct Snap {
    uint32_t st, b18, b1C, b20, b24, o1C, o24, o28, o2C, slot;
    uint16_t o20;
    void Take(uint32_t state, uint32_t slotva) {
        const uint32_t o = R32(state);
        st = R32(state + 0x20);
        b18 = R32(state + 0x48); b1C = R32(state + 0x4C); b20 = R32(state + 0x50); b24 = R32(state + 0x54);
        o1C = R32(o + 0x1C); o20 = R16(o + 0x20); o24 = R32(o + 0x24); o28 = R32(o + 0x28); o2C = R32(o + 0x2C);
        slot = R32(slotva);
    }
    void Put(uint32_t state, uint32_t slotva) const {
        const uint32_t o = R32(state);
        W32(state + 0x20, st);
        W32(state + 0x48, b18); W32(state + 0x4C, b1C); W32(state + 0x50, b20); W32(state + 0x54, b24);
        W32(o + 0x1C, o1C); W16(o + 0x20, o20); W32(o + 0x24, o24); W32(o + 0x28, o28); W32(o + 0x2C, o2C);
        W32(slotva, slot);
    }
    bool Same(const Snap& b) const {
        return st == b.st && b18 == b.b18 && b1C == b.b1C && b20 == b.b20 && b24 == b.b24 && o1C == b.o1C &&
               o20 == b.o20 && o24 == b.o24 && o28 == b.o28 && o2C == b.o2C && slot == b.slot;
    }
};
}  // namespace

// stdcall entries: on entry ctx.esp points at the return address, the arguments above it
X86_EXTERN_FUNC(__imp__sub_0042B2D9);
X86_FUNC(sub_0042B2D9) {
    if (kCheck) {  // the check compares against the game's code alone
        __imp__sub_0042B2D9(ctx, base);
        return;
    }
    const uint32_t esp0 = ctx.esp;
    const uint32_t s = R32(esp0 + 4), n = R32(esp0 + 8), outp = R32(esp0 + 12);
    ctx.esp = esp0 - 16;
    uint32_t v = 0;
    const uint32_t eax = Peek(ctx, s, n, v, esp0 - 16);
    if (int32_t(eax) >= 0 && eax != kErr) W32(outp, v);
    ctx.esp = esp0 + 16;
    ctx.eax = eax;
}

X86_EXTERN_FUNC(__imp__sub_0042B3CC);
X86_FUNC(sub_0042B3CC) {
    if (kCheck) {
        __imp__sub_0042B3CC(ctx, base);
        return;
    }
    const uint32_t esp0 = ctx.esp;
    const uint32_t s = R32(esp0 + 4), n = R32(esp0 + 8);
    ctx.esp = esp0 - 16;
    const uint32_t eax = Skip(ctx, s, n, esp0 - 16);
    ctx.esp = esp0 + 12;
    ctx.eax = eax;
}

X86_EXTERN_FUNC(__imp__sub_00430646);
X86_FUNC(sub_00430646) {
    if (kCheck) {
        __imp__sub_00430646(ctx, base);
        return;
    }
    const uint32_t esp0 = ctx.esp;
    const uint32_t table = R32(esp0 + 4), bits = R32(esp0 + 8);
    const uint32_t pl = R32(esp0 + 12), ps = R32(esp0 + 16), pr = R32(esp0 + 20);
    ctx.esp = esp0 - 16;
    uint32_t len = 0, sym = 0, rest = 0;
    const uint32_t eax = Huffman(ctx, table, bits, len, sym, pr ? &rest : NULL, esp0 - 16);
    if (int32_t(eax) >= 0) {
        W32(pl, len);
        W32(ps, sym);
        if (pr) W32(pr, rest);
    }
    ctx.esp = esp0 + 24;
    ctx.eax = eax;
}

// dequant check trace: per coefficient call, the position at entry and the run, level, sign after
int g_trace_phase = 0;
uint32_t g_trace_n[3];
uint32_t g_trace[3][600][4];
uint32_t g_trace_in[3][600][6];  // at entry: reader pointer, window, count, st, tables, third argument
uint32_t g_trace_tables, g_trace_arg3;
static void Trace(uint32_t state, int when) {
    if (!g_trace_phase || !state) return;
    const uint32_t o = R32(state);
    uint32_t& n = g_trace_n[g_trace_phase];
    if (n >= 600) return;
    if (when == 0) {
        g_trace[g_trace_phase][n][0] = R16(o + 0x74);
        g_trace_in[g_trace_phase][n][0] = R32(state + 0x48);
        g_trace_in[g_trace_phase][n][1] = R32(state + 0x50);
        g_trace_in[g_trace_phase][n][2] = R32(state + 0x54);
        g_trace_in[g_trace_phase][n][3] = R32(state + 0x20);
        g_trace_in[g_trace_phase][n][4] = g_trace_tables;
        g_trace_in[g_trace_phase][n][5] = g_trace_arg3;
        return;
    }
    g_trace[g_trace_phase][n][1] = R32(o + 0x1C);
    g_trace[g_trace_phase][n][2] = R16(o + 0x20);
    g_trace[g_trace_phase][n][3] = R32(o + 0x24);
    n++;
}
X86_EXTERN_FUNC(__imp__sub_0042D66A);
X86_FUNC(sub_0042D66A) {
    const uint32_t esp0 = ctx.esp;
    const uint32_t state = R32(esp0 + 4), tables = R32(esp0 + 8);
    struct TraceScope {
        uint32_t s;
        TraceScope(uint32_t st) : s(st) { Trace(s, 0); }
        ~TraceScope() { Trace(s, 1); }
    } trace_scope(kCheck ? (g_trace_tables = tables, g_trace_arg3 = R32(esp0 + 12), state) : 0);  // the dequant check only
    const uint32_t slot = esp0 + 4;
    if (kCheck && int32_t(R32(state + 0x4C)) >= 16) {  // no refill inside: the stream's bytes are not read again
        Snap before, game, mine;
        before.Take(state, slot);
        __imp__sub_0042D66A(ctx, base);
        const uint32_t game_eax = ctx.eax;
        game.Take(state, slot);
        before.Put(state, slot);
        ctx.esp = esp0 - 16;
        const uint32_t my_eax = Coefficient(ctx, state, tables, slot, esp0 - 16);
        mine.Take(state, slot);
        g_checked++;
        if (!game.Same(mine) || game_eax != my_eax) {
            if (g_differ++ < 20)
                LOGW("wma: differs at %u: state %X eax %X/%X window %X/%X bits %u/%u o1C %X/%X o20 %X/%X o24 %X/%X", g_checked,
                     state, game_eax, my_eax, game.b20, mine.b20, game.b24, mine.b24, game.o1C, mine.o1C, game.o20,
                     mine.o20, game.o24, mine.o24);
            game.Put(state, slot);
        }
        if ((g_checked & 0xFFFF) == 0) LOGI("wma: %u coefficients checked, %u differ", g_checked, g_differ);
        ctx.esp = esp0 + 16;
        ctx.eax = game_eax;
        return;
    }
    ctx.esp = esp0 - 16;
    const uint32_t eax = Coefficient(ctx, state, tables, slot, esp0 - 16);
    ctx.esp = esp0 + 16;
    ctx.eax = eax;
}

// MC3 (2026-11-19): the inverse quantization (0x42A37F(decoder, state, frame, unused), ret 0x10), native. Console log
// 05:50: 0.18 ms a call, two calls a block. Per coefficient the game's code read the run and level the reader left in
// the decoder, turned the level into a float (fild from a stack slot just written: a load-hit-store), scaled it and
// stored it into the frame's float array (a float store through the byte reversed path: another), and the scale came
// from 0x42A345 per band (table[e] / 2^(e/4) * decoder[0]). Here the coefficient reader is called directly (the native
// one above, when [decoder+0x3F4] is 0x42D66A), levels come from a table of doubles, and the stores are kept in host
// arrays and written to the guest after the loop, before the zeroed ranges (the game zeroes after its stores too).
// game:\wmacheck.txt: the game's code runs first (blocks whose stream refilled inside are left to it) and its
// output array and decoder fields are compared ("wma dequant: N blocks checked ...").
extern int64_t g_mc3_wma_ns[10];
extern uint32_t g_mc3_wma_calls[10];
namespace {
const uint32_t kDequantErr = 0x80040002u;
double g_lev[2048];  // int16 levels -1024..1023 as doubles
double g_pow2[32];   // fild of (1 << s) as a signed 32-bit integer
struct DqInit {
    DqInit() {
        for (int i = 0; i < 2048; i++) g_lev[i] = double(i - 1024);
        for (int s = 0; s < 32; s++) g_pow2[s] = double(int32_t(1u << s));
    }
} g_dq_init;
const int kMaxDq = 4096;
uint32_t g_dq_pos[kMaxDq];
union {
    float f[kMaxDq];
    uint32_t u[kMaxDq];
} g_dq_val;
uint32_t g_dq_n = 0;
uint32_t g_dq_native = 0, g_dq_checked = 0, g_dq_differ = 0, g_dq_skipped = 0;

inline double Fl(uint32_t bits) {
    union { uint32_t u; float f; } c;
    c.u = bits;
    return double(c.f);
}
inline double Level(uint32_t obj) {
    const int32_t l = int16_t(R16(obj + 0x20));
    if (l >= -1024 && l < 1024) return g_lev[l + 1024];
    return double(l);
}
// 0x42A345(decoder, frame, band): the band's scale, an x87 value
double BandScale(uint32_t obj, uint32_t fr, int32_t band) {
    int32_t e = int32_t(R32(fr + 0x34)) - int32_t(R32(R32(fr + 4) + uint32_t(band) * 4));
    if (e >= 0x48) e = 0x47;
    const double t = Fl(R32(0x00439448u + uint32_t(e) * 4));
    return t / g_pow2[(e >> 2) & 31] * Fl(R32(obj));
}
void DqFlush(uint32_t out) {
    for (uint32_t k = 0; k < g_dq_n; k++) W32(out + g_dq_pos[k] * 4, g_dq_val.u[k]);
    g_dq_n = 0;
}
// The stream's bytes can sit in the frame's float array ahead of the stores (Xenia check 2026-11-19: the game read
// back floats it had just stored), so a store into the bytes not read yet goes to the guest at once, and the kept ones
// go before any coefficient that may refill (a refill's new bytes come after the game's stores).
uint32_t g_dq_now = 0, g_dq_later = 0;
inline void DqStore(uint32_t out, int32_t pos, double v, uint32_t state) {
    const uint32_t a = out + uint32_t(pos) * 4, p = R32(state + 0x48), left = R32(state + 0x4C);
    if (a + 4 > p && a < p + left + 4) {
        g_dq_val.f[kMaxDq - 1] = float(v);
        W32(a, g_dq_val.u[kMaxDq - 1]);
        g_dq_now++;
        return;
    }
    if (g_dq_n == uint32_t(kMaxDq - 1)) DqFlush(out);
    g_dq_pos[g_dq_n] = uint32_t(pos);
    g_dq_val.f[g_dq_n] = float(v);
    g_dq_n++;
    g_dq_later++;
}
inline void DqBeforeCoef(uint32_t out, uint32_t state) {
    if (g_dq_n && int32_t(R32(state + 0x4C)) < 16) DqFlush(out);
}

uint32_t Dequant(X86Context& ctx, uint32_t obj, uint32_t state, uint32_t fr, uint32_t a4, uint32_t slot, uint32_t scratch) {
    const uint32_t out = R32(fr + 0x30);
    const int32_t n = int32_t(R32(obj + 0x7C));
    if (n <= 0) return kDequantErr;
    const int32_t d = int32_t(R32(obj + R32(obj + 0x70) * 4 + 0x178));
    if (d <= 0) return kDequantErr;
    const int32_t ratio = int32_t(uint32_t(n) << 12) / d;
    int32_t lgr = 0;
    if (uint32_t(ratio) > 1) do lgr++; while ((uint32_t(ratio) >> (lgr & 31)) > 1);
    const int32_t q = int32_t(R32(obj + 0x31C)) / n;
    uint32_t lg = 0;
    if (uint32_t(q) > 1) do lg++; while ((uint32_t(q) >> (lg & 31)) > 1);
    const uint32_t bands = lg * 0x68 + R32(obj + 0x3B0);
    const int32_t nb = int32_t(R32(R32(obj + 0x3AC) + lg * 4));
    const uint32_t coef = R32(obj + 0x3F4);
    const bool direct = coef == 0x0042D66A && !kCheck;
#define BSTART(i) int32_t(R32(bands + 4 + 4 * uint32_t(i)))
#define COEF() (DqBeforeCoef(out, state), direct ? Coefficient(ctx, state, fr, slot, scratch) : kernel::CallGuest(ctx, coef, state, fr, a4))
    W16(obj + 0x20, 0);
    W32(obj + 0x28, R32(fr + 0x20) - 1);
    W32(obj + 0x2C, 0xFFFFFFFFu);
    int32_t band = 0;
    {
        const int32_t pr = (int32_t(int16_t(R16(obj + 0x74))) * ratio) >> 12;
        while (pr >= BSTART(band)) band++;
    }
    g_dq_n = 0;
    uint32_t rc = COEF();
    if (int32_t(rc) < 0) return rc;
    W16(obj + 0x74, uint16_t(R16(obj + 0x74) + uint16_t(R16(obj + 0x1C) + 1)));
    int32_t pos = int16_t(R16(obj + 0x74));
    float scale = float(Fl(obj));  // the game's [ebp+8] before its first band: the decoder pointer's bits
    if (pos < int32_t(R32(obj + 0x28))) {
        for (;;) {
            if (band >= nb) break;
            {
                const int32_t pr = (pos * ratio) >> 12;
                while (pr >= BSTART(band)) band++;
            }
            if (band >= nb) break;
            double val;
            if (pos == int32_t(R32(obj + 0x2C))) {
                val = Level(obj) * double(scale);
                scale = float(BandScale(obj, fr, band));
            } else {
                scale = float(BandScale(obj, fr, band));
                val = Level(obj) * double(scale);
            }
            int32_t end;
            if (lgr > 12) end = ((1 << ((lgr - 13) & 31)) + BSTART(band)) >> ((lgr - 12) & 31);
            else end = BSTART(band) << ((12 - lgr) & 31);
            end--;
            if (end > int32_t(R32(obj + 0x28))) end = int32_t(R32(obj + 0x28));
            W32(obj + 0x2C, uint32_t(end));
            for (;;) {
                if (R32(obj + 0x24) != 0) val = -val;
                DqStore(out, int16_t(R16(obj + 0x74)), val, state);
                rc = COEF();
                if (int32_t(rc) < 0) {
                    DqFlush(out);
                    return rc;
                }
                W16(obj + 0x74, uint16_t(R16(obj + 0x74) + uint16_t(R16(obj + 0x1C) + 1)));
                pos = int16_t(R16(obj + 0x74));
                if (pos < int32_t(R32(obj + 0x2C))) {
                    val = Level(obj) * double(scale);
                    continue;
                }
                break;
            }
            band++;
            if (!(pos < int32_t(R32(obj + 0x28)))) break;
        }
    }
    pos = int16_t(R16(obj + 0x74));
    if (pos == int32_t(R32(obj + 0x28))) {
        if (pos >= int32_t(R32(obj + 0x2C))) {
            int32_t c = band - 1;
            if (c < nb) {
                const int32_t pr = (pos * ratio) >> 12;
                while (!(pr < int32_t(R32(bands + uint32_t(band) * 4)))) {
                    band++;
                    c++;
                    if (!(c < nb)) break;
                }
            }
            band--;
            if (band <= nb) scale = float(BandScale(obj, fr, band));
        }
        double val = Level(obj) * double(scale);
        if (R32(obj + 0x24) != 0) val = -val;
        DqStore(out, pos, val, state);
    }
    DqFlush(out);
#undef BSTART
#undef COEF
    const int32_t z = int32_t(R32(obj + 0x324));
    for (int32_t i = 0; i < z; i++) W32(out + 4 * uint32_t(i), 0);
    const uint32_t from = R32(obj + 0x328);
    const int32_t cnt = int32_t(R32(obj + 0x60)) - int32_t(from);
    for (int32_t i = 0; i < cnt; i++) W32(out + (from + uint32_t(i)) * 4, 0);
    return rc;
}

struct DqSnap {
    uint32_t st, b18, b1C, b20, b24, o1C, o24, o28, o2C;
    uint16_t o20, o74;
    void Take(uint32_t obj, uint32_t state) {
        st = R32(state + 0x20);
        b18 = R32(state + 0x48); b1C = R32(state + 0x4C); b20 = R32(state + 0x50); b24 = R32(state + 0x54);
        o1C = R32(obj + 0x1C); o20 = R16(obj + 0x20); o24 = R32(obj + 0x24); o28 = R32(obj + 0x28); o2C = R32(obj + 0x2C);
        o74 = R16(obj + 0x74);
    }
    void Put(uint32_t obj, uint32_t state) const {
        W32(state + 0x20, st);
        W32(state + 0x48, b18); W32(state + 0x4C, b1C); W32(state + 0x50, b20); W32(state + 0x54, b24);
        W32(obj + 0x1C, o1C); W16(obj + 0x20, o20); W32(obj + 0x24, o24); W32(obj + 0x28, o28); W32(obj + 0x2C, o2C);
        W16(obj + 0x74, o74);
    }
    bool Same(const DqSnap& b) const {
        return st == b.st && b18 == b.b18 && b1C == b.b1C && b20 == b.b20 && b24 == b.b24 && o1C == b.o1C && o20 == b.o20 &&
               o24 == b.o24 && o28 == b.o28 && o2C == b.o2C && o74 == b.o74;
    }
};
uint32_t g_dq_save[kMaxDq], g_dq_ref[kMaxDq], g_dq_args[4];
uint8_t g_dq_buf[0x200];
}  // namespace

X86_EXTERN_FUNC(__imp__sub_0042A37F);
X86_FUNC(sub_0042A37F) {
    const int64_t t0 = host::NowNs();
    const uint32_t esp0 = ctx.esp;
    const uint32_t obj = R32(esp0 + 4), state = R32(esp0 + 8), fr = R32(esp0 + 12), a4 = R32(esp0 + 16);
    const uint32_t ebx = ctx.ebx, esi = ctx.esi, edi = ctx.edi, ebp = ctx.ebp;
    const uint32_t out = R32(fr + 0x30);
    const uint32_t nout = R32(obj + 0x60);
    bool check = kCheck && nout <= uint32_t(kMaxDq);
    bool game_only = kCheck && !check;
    DqSnap before, game, mine;
    uint32_t game_eax = 0;
    if (check) {
        before.Take(obj, state);
        for (uint32_t i = 0; i < nout; i++) g_dq_save[i] = R32(out + 4 * i);
        // the stream's bytes live in a buffer inside the state (a refill copies the next packet's bytes there), and
        // the game's code writes its own argument slots (the scale, a level): both kept to compare and put back
        for (uint32_t i = 0; i < 0x200; i++) g_dq_buf[i] = kernel::G8(state + i);
        for (uint32_t i = 0; i < 4; i++) g_dq_args[i] = R32(esp0 + 4 + 4 * i);
        g_trace_n[1] = g_trace_n[2] = 0;
        g_trace_phase = 1;
        __imp__sub_0042A37F(ctx, base);
        g_trace_phase = 0;
        game_eax = ctx.eax;
        game.Take(obj, state);
        bool refilled = game.b18 - before.b18 > before.b1C;
        // the bytes between the reader's fields and the decoder (which follows the state in memory): the stream buffer
        const uint32_t scan_end = obj > state && obj - state < 0x200 ? obj - state : 0x200;
        for (uint32_t i = 0x58; i < scan_end && !refilled; i++)
            if (kernel::G8(state + i) != g_dq_buf[i]) refilled = true;
        if (refilled) {
            // the stream refilled inside (a callback hands the next packet): keep the game's result as it is
            game_only = true;
            g_dq_skipped++;
        } else {
            for (uint32_t i = 0; i < nout; i++) {
                g_dq_ref[i] = R32(out + 4 * i);
                W32(out + 4 * i, g_dq_save[i]);
            }
            before.Put(obj, state);
            for (uint32_t i = 0; i < 4; i++) W32(esp0 + 4 + 4 * i, g_dq_args[i]);
            ctx.esp = esp0;
        }
    }
    if (game_only) {
        if (!check) __imp__sub_0042A37F(ctx, base);
    } else {
        ctx.esp = esp0 - 64;
        if (check) g_trace_phase = 2;
        static const bool kSame = GetFileAttributesA("game:\dqsame.txt") != DWORD(-1);
        uint32_t eax;
        if (check && kSame) {  // the harness itself: the game's code twice on the same state
            ctx.esp = esp0;
            __imp__sub_0042A37F(ctx, base);
            eax = ctx.eax;
        } else {
            eax = Dequant(ctx, obj, state, fr, a4, esp0 - 8, esp0 - 64);
        }
        g_trace_phase = 0;
        g_dq_native++;
        ctx.eax = eax;
        ctx.ebx = ebx;
        ctx.esi = esi;
        ctx.edi = edi;
        ctx.ebp = ebp;
        ctx.esp = esp0 + 20;
        if (check) {
            mine.Take(obj, state);
            uint32_t bad = 0;
            for (uint32_t i = 0; i < nout; i++)
                if (R32(out + 4 * i) != g_dq_ref[i]) bad++;
            if (!mine.Same(game) || eax != game_eax) bad += 100000;
            g_dq_checked++;
            if (bad) {
                if (g_dq_differ++ < 10)
                {
                    uint32_t first = 0, nz_game = 0, nz_mine = 0;
                    for (uint32_t i = 0; i < nout; i++) {
                        if (g_dq_ref[i]) nz_game++;
                        if (R32(out + 4 * i)) nz_mine++;
                    }
                    while (first < nout && R32(out + 4 * first) == g_dq_ref[first]) first++;
                    LOGW("wma dequant: block %u differs (%u values%s), eax %X/%X pos %u/%u; first at %u: %08X/%08X, next %08X/%08X; "
                         "nonzero %u/%u; st %X/%X ptr %X/%X left %X/%X win %X/%X n %X/%X o1C %X/%X o20 %X/%X o24 %X/%X o28 %X/%X o2C %X/%X",
                         g_dq_checked, bad % 100000, bad >= 100000 ? ", fields" : "", eax, game_eax, mine.o74, game.o74, first,
                         first < nout ? R32(out + 4 * first) : 0, first < nout ? g_dq_ref[first] : 0,
                         first + 1 < nout ? R32(out + 4 * first + 4) : 0, first + 1 < nout ? g_dq_ref[first + 1] : 0, nz_mine, nz_game,
                         mine.st, game.st, mine.b18, game.b18, mine.b1C, game.b1C, mine.b20, game.b20, mine.b24, game.b24, mine.o1C,
                         game.o1C, mine.o20, game.o20, mine.o24, game.o24, mine.o28, game.o28, mine.o2C, game.o2C);
                }
                if (g_dq_differ <= 2) {
                    uint32_t k = 0;
                    const uint32_t m = g_trace_n[1] < g_trace_n[2] ? g_trace_n[1] : g_trace_n[2];
                    while (k < m && !memcmp(g_trace[1][k], g_trace[2][k], 16)) k++;
                    LOGW("wma dequant trace: %u/%u calls, first different at %u; obj %X state %X frame %X out %X n %u", g_trace_n[2], g_trace_n[1], k, obj, state, fr, out, nout);
                    for (uint32_t j = (k > 3 ? k - 3 : 0); j < k + 4 && j < 600; j++)
                    {
                        LOGW("  %u: mine pos %u run %X lev %d sign %X | game pos %u run %X lev %d sign %X", j, g_trace[2][j][0],
                             g_trace[2][j][1], int16_t(g_trace[2][j][2]), g_trace[2][j][3], g_trace[1][j][0], g_trace[1][j][1],
                             int16_t(g_trace[1][j][2]), g_trace[1][j][3]);
                        LOGW("     in mine ptr %X win %X n %u st %X tab %X a3 %X | game ptr %X win %X n %u st %X tab %X a3 %X",
                             g_trace_in[2][j][0], g_trace_in[2][j][1], g_trace_in[2][j][2], g_trace_in[2][j][3], g_trace_in[2][j][4],
                             g_trace_in[2][j][5], g_trace_in[1][j][0], g_trace_in[1][j][1], g_trace_in[1][j][2], g_trace_in[1][j][3],
                             g_trace_in[1][j][4], g_trace_in[1][j][5]);
                    }
                }
                for (uint32_t i = 0; i < nout; i++) W32(out + 4 * i, g_dq_ref[i]);
                game.Put(obj, state);
                ctx.eax = game_eax;
            }
            if ((g_dq_checked & 0xFF) == 0) LOGI("wma dequant: %u blocks checked, %u differ (%u with a refill left to the game); stores at once %u, kept %u", g_dq_checked, g_dq_differ, g_dq_skipped, g_dq_now, g_dq_later);
        }
    }
    g_mc3_wma_ns[4] += host::NowNs() - t0;
    g_mc3_wma_calls[4]++;
}
