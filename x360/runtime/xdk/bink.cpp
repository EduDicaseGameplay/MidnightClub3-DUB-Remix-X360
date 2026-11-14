// Xbox 360 build of the PC port's runtime/xdk/bink.cpp (MC3, 2026-11-11).
// BinkGetSummary divides bytes read * 1000 by the read time in ms + 1. Host reads take under a millisecond,
// the time stays 0 and the quotient of a large movie overflows the 32 bit div. Raise the time just enough.
// Movies step one frame per game frame, timed by the console's 30 Hz present. A frame is decoded and advanced only
// once BinkWait, which follows the movie's own rate and audio, says it is due: no decode the movie does not show.
#include <set>

#include "xdk/hle.h"

using namespace kernel;

#if HAS_BinkGetSummary
X86_EXTERN_FUNC(GUEST_ORIG(BinkGetSummary));
X86_FUNC(GUEST(BinkGetSummary)) {
    uint32_t bink = R32(ctx.esp + 4);
    if (bink) {
        // 0x134 bytes read, 0x13C read time in ms
        uint64_t scaled = uint64_t(R32(bink + 0x134)) * 1000;
        uint32_t time = R32(bink + 0x13C);
        if (scaled >> 32 > time) W32(bink + 0x13C, uint32_t(scaled >> 32));
    }
    GUEST_ORIG(BinkGetSummary)(ctx, base);
}
#endif

#if HAS_BinkWait && HAS_BinkNextFrame && HAS_BinkDoFrame
namespace {
const uint32_t kBinkWait = SYM(BinkWait);
CRITICAL_SECTION g_held_lock;
struct HeldInit {
    HeldInit() { InitializeCriticalSection(&g_held_lock); }
} g_held_init;
std::set<uint32_t> g_held;  // movies whose frame was not decoded this game frame
}  // namespace

X86_EXTERN_FUNC(GUEST_ORIG(BinkDoFrame));
X86_FUNC(GUEST(BinkDoFrame)) {
    uint32_t bink = R32(ctx.esp + 4);
    if (bink && CallGuest(ctx, kBinkWait, bink)) {
        EnterCriticalSection(&g_held_lock);
        g_held.insert(bink);
        LeaveCriticalSection(&g_held_lock);
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    GUEST_ORIG(BinkDoFrame)(ctx, base);
}

X86_EXTERN_FUNC(GUEST_ORIG(BinkNextFrame));
X86_FUNC(GUEST(BinkNextFrame)) {
    uint32_t bink = R32(ctx.esp + 4);
    EnterCriticalSection(&g_held_lock);
    bool held = g_held.erase(bink) != 0;
    LeaveCriticalSection(&g_held_lock);
    if (held) {
        ctx.esp += 8;
        return;
    }
    GUEST_ORIG(BinkNextFrame)(ctx, base);
}
#endif
