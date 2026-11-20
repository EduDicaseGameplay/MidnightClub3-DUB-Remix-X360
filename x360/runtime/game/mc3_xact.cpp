// MC3 (2026-11-18): XACT's work (XACTEngineDoWork, 0x4073D9) as often as the console's frames would run it.
//
// The engine's work, once a frame: DirectSound's own work (0x3C22EC: the packet callbacks), then the engine
// (0x407320), which decodes the songs (WMA) and hands their packets to the streams (CDirectSoundStream_Process from
// 0x40F757, through 0x40D70F, 0x40CDCE, 0x40CB94, 0x40C232, 0x40F735). Each call decodes a slice; at 30 fps that keeps
// ahead of the music. Here a frame takes ~100 ms on the console and the music ran out ~1.6 times a second for ~90 ms
// with its next packet late (console log of 2026-11-18 04:32, "streams refilled late 8 times (742 ms silent)" every
// 5 s), heard as stutters in the pause menu where nothing covers them. So a call that comes more than 40 ms after the
// last runs the work again, once per 33 ms that passed (at most 3 times in all): the decoding keeps up with the music
// as it did at 30 fps. With the streams full the extra calls find nothing to do.
//
// An earlier try (handing music packets back half way, from a copy) froze the game in Xenia: XACT spins on its own
// packet accounting. This leaves the packets alone and only gives XACT its time.
#include <stdio.h>
#include <string.h>

#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "host.h"

namespace {
int64_t g_last_ns = 0;
uint32_t g_extra = 0, g_calls = 0;
int64_t g_report_ns = 0, g_first_ns = 0, g_extra_ns = 0;  // time in the first run and in the extra ones
bool g_inside = false;
// MC3 (2026-11-18): the engine's part (0x407320); the rest is DirectSound's work (0x3C22EC, DirectSoundDoWork, which the
// runtime replaces: not hooked here, a hook calling the game's own code froze the boot in Xenia)
int64_t g_engine_ns = 0;
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00407320);
X86_FUNC(sub_00407320) {
    const int64_t t0 = host::NowNs();
    __imp__sub_00407320(ctx, base);
    g_engine_ns += host::NowNs() - t0;
}

// MC3 (2026-11-19): the WMA decoder's block-level functions, timed (inclusive; each runs a few hundred times in 10 s)
namespace {
const char* const kWmaNames[] = {"4288BC", "428DAB", "42DD91", "42A2D4", "42A37F", "42A756", "42F5CC", "42B5C2", "42A972", "42D66A"};
}  // namespace
int64_t g_mc3_wma_ns[10];  // also used by game/mc3_imdct.cpp
uint32_t g_mc3_wma_calls[10];
X86_EXTERN_FUNC(__imp__sub_004288BC);
X86_FUNC(sub_004288BC) {
    const int64_t t0 = host::NowNs();
    __imp__sub_004288BC(ctx, base);
    g_mc3_wma_ns[0] += host::NowNs() - t0;
    g_mc3_wma_calls[0]++;
}
X86_EXTERN_FUNC(__imp__sub_00428DAB);
X86_FUNC(sub_00428DAB) {
    const int64_t t0 = host::NowNs();
    __imp__sub_00428DAB(ctx, base);
    g_mc3_wma_ns[1] += host::NowNs() - t0;
    g_mc3_wma_calls[1]++;
}
X86_EXTERN_FUNC(__imp__sub_0042DD91);
X86_FUNC(sub_0042DD91) {
    const int64_t t0 = host::NowNs();
    __imp__sub_0042DD91(ctx, base);
    g_mc3_wma_ns[2] += host::NowNs() - t0;
    g_mc3_wma_calls[2]++;
}
X86_EXTERN_FUNC(__imp__sub_0042A2D4);
X86_FUNC(sub_0042A2D4) {
    const int64_t t0 = host::NowNs();
    __imp__sub_0042A2D4(ctx, base);
    g_mc3_wma_ns[3] += host::NowNs() - t0;
    g_mc3_wma_calls[3]++;
}
// 0x42A37F: native in game/mc3_wma.cpp, which times itself in slot 4
X86_EXTERN_FUNC(__imp__sub_0042A756);
X86_FUNC(sub_0042A756) {
    const int64_t t0 = host::NowNs();
    __imp__sub_0042A756(ctx, base);
    g_mc3_wma_ns[5] += host::NowNs() - t0;
    g_mc3_wma_calls[5]++;
}
// 0x42F5CC: native in game/mc3_imdct.cpp, which times itself in slot 6
// 0x42B5C2: native in game/mc3_imdct.cpp, which times itself in slot 7
X86_EXTERN_FUNC(__imp__sub_0042A972);
X86_FUNC(sub_0042A972) {
    const int64_t t0 = host::NowNs();
    __imp__sub_0042A972(ctx, base);
    g_mc3_wma_ns[8] += host::NowNs() - t0;
    g_mc3_wma_calls[8]++;
}

// MC3 (2026-11-19): the WMA media object's Process (vtable 0x4A5594 slot 4: this, in packet, out packet; ret 0xC) and
// Flush (slot 6). Console log 2026-11-19 04:42: the decoder ran in bursts (1547 blocks, 2.5 s, in one 10 s window, fps
// 4.6) when the garage's song changed. Who asks, and for how much.
namespace {
uint32_t g_proc_calls = 0, g_proc_flush = 0, g_proc_slow = 0;
uint64_t g_proc_bytes = 0;
uint32_t g_proc_max = 0;
int64_t g_proc_ns = 0;
uint32_t g_proc_ret[8], g_proc_retn[8];
}  // namespace
X86_EXTERN_FUNC(__imp__sub_00425F87);
X86_FUNC(sub_00425F87) {
    const uint32_t esp = ctx.esp;
    const uint32_t ret = kernel::R32(esp), self = kernel::R32(esp + 4), out = kernel::R32(esp + 12);
    const uint32_t size = out ? kernel::R32(out + 4) : 0;
    const int64_t t0 = host::NowNs();
    __imp__sub_00425F87(ctx, base);
    const int64_t dt = host::NowNs() - t0;
    g_proc_ns += dt;
    g_proc_calls++;
    g_proc_bytes += size;
    if (size > g_proc_max) g_proc_max = size;
    for (int i = 0; i < 8; i++) {
        if (g_proc_ret[i] == ret || !g_proc_ret[i]) { g_proc_ret[i] = ret; g_proc_retn[i]++; break; }
    }
    if (dt > 30000000ll && g_proc_slow < 12) {
        g_proc_slow++;
        // the caller's frame chain: ebp at entry is the caller's frame
        LOGI("wma process: slow call %.1f ms, object %08X, %u bytes asked, from %08X (frames %08X %08X %08X)",
             double(dt) / 1e6, self, size, ret, kernel::R32(ctx.ebp + 4), kernel::R32(kernel::R32(ctx.ebp) + 4),
             kernel::R32(kernel::R32(kernel::R32(ctx.ebp)) + 4));
    }
}
X86_EXTERN_FUNC(__imp__sub_00425F23);
X86_FUNC(sub_00425F23) {
    g_proc_flush++;
    LOGI("wma process: flush of object %08X from %08X", kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp));
    __imp__sub_00425F23(ctx, base);
}
static void ReportProcess() {
    if (!g_proc_calls && !g_proc_flush) return;
    char sb[200];
    int so = 0;
    for (int i = 0; i < 8 && g_proc_ret[i]; i++) so += sprintf(sb + so, " %08X:%u", g_proc_ret[i], g_proc_retn[i]);
    LOGI("wma process: %u calls in 10 s (%.1f ms), %llu KB asked, the largest %u bytes, %u flushes; from%s", g_proc_calls,
         double(g_proc_ns) / 1e6, (unsigned long long)(g_proc_bytes / 1024), g_proc_max, g_proc_flush, sb);
    g_proc_calls = g_proc_flush = g_proc_slow = g_proc_max = 0;
    g_proc_bytes = 0;
    g_proc_ns = 0;
    memset(g_proc_ret, 0, sizeof(g_proc_ret));
    memset(g_proc_retn, 0, sizeof(g_proc_retn));
}

X86_EXTERN_FUNC(__imp__sub_004073D9);
X86_FUNC(sub_004073D9) {
    const int64_t now = host::NowNs();
    int runs = 1;
    if (!g_inside && g_last_ns) {
        const int64_t gap = now - g_last_ns;
        if (gap > 40000000ll) {
            runs = int(gap / 33333333ll);
            if (runs < 1) runs = 1;
            if (runs > 3) runs = 3;
        }
    }
    g_last_ns = now;
    const bool outer = !g_inside;
    g_inside = true;
    const uint32_t esp = ctx.esp;
    for (int i = 0; i < runs; i++) {
        ctx.esp = esp;  // the same call again, as the caller made it (no arguments, cdecl)
        const int64_t t0 = host::NowNs();
        __imp__sub_004073D9(ctx, base);
        (i ? g_extra_ns : g_first_ns) += host::NowNs() - t0;
        if (i + 1 < runs) {
            // __imp_ returned (popped its return address): put it back for the next run
            ctx.esp = esp;
        }
    }
    if (outer) g_inside = false;
    g_calls++;
    g_extra += uint32_t(runs - 1);
    if (!g_report_ns) g_report_ns = now;
    if (now - g_report_ns >= 10000000000ll) {
        LOGI("xact: %u calls of the engine's work in 10 s (%.1f ms), %u more run for long frames (%.1f ms); of all, "
             "the engine (songs, cues) %.1f ms, DirectSound's work and the rest %.1f ms", g_calls,
             double(g_first_ns) / 1e6, g_extra, double(g_extra_ns) / 1e6, double(g_engine_ns) / 1e6,
             double(g_first_ns + g_extra_ns - g_engine_ns) / 1e6);
        g_calls = g_extra = 0;
        g_first_ns = g_extra_ns = 0;
        g_engine_ns = 0;
        {
            char sb[400];
            int so = 0;
            for (int i = 0; i < int(sizeof(g_mc3_wma_ns) / sizeof(g_mc3_wma_ns[0])) && so < 360; i++)
                if (g_mc3_wma_calls[i]) so += sprintf(sb + so, " %s:%u %.1f", kWmaNames[i], g_mc3_wma_calls[i], double(g_mc3_wma_ns[i]) / 1e6);
            if (so) LOGI("xact: wma blocks (calls, ms):%s", sb);
            memset(g_mc3_wma_ns, 0, sizeof(g_mc3_wma_ns));
            memset(g_mc3_wma_calls, 0, sizeof(g_mc3_wma_calls));
        }
        ReportProcess();
        g_report_ns = now;
    }
}
