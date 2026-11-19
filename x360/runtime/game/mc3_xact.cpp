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
        g_report_ns = now;
    }
}
