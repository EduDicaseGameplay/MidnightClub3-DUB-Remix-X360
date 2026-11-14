// MC3 (2026-11-14): traffic and pedestrian density scale, from the PC reference's world_tuning.cpp (same project; there
// it raises the density, here it can lower it). Every traffic car is AI, physics and dozens of draws a frame, and the
// title thread's frame time follows the draw count (ANALISE_FPS.md): fewer cars and peds, more frames.
//   0x1CB290 sends the level's ambient density to each road's setter 0x2B9670 (thiscall road, float density) and the
//   pedestrian density to 0x222770 (thiscall manager, float density). The density argument is scaled for the call;
//   the game's own ceiling (0x2B9690, applied after) only limits the most, so a lower value passes as it is.
//   game:\trafego.txt: "traffic peds", each 0.2 to 1.0 (1.0 is the console); without it kDefault.
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using kernel::R32;
using kernel::W32;

namespace {
const float kDefaultTraffic = 0.7f, kDefaultPeds = 0.7f;
float g_traffic = -1, g_peds = -1;

void Load() {
    if (g_traffic >= 0) return;
    float t = kDefaultTraffic, p = kDefaultPeds;
    if (FILE* f = fopen("game:\\trafego.txt", "rb")) {
        char b[64] = {0};
        fread(b, 1, sizeof(b) - 1, f);
        fclose(f);
        char* e = NULL;
        double a = strtod(b, &e);
        if (a > 0) t = float(a);
        double c = e ? strtod(e, NULL) : 0;
        p = c > 0 ? float(c) : t;
    }
    if (!_finite(t) || t < 0.2f) t = t < 0.2f ? 0.2f : 1.0f;
    if (!_finite(p) || p < 0.2f) p = p < 0.2f ? 0.2f : 1.0f;
    if (t > 1.0f) t = 1.0f;
    if (p > 1.0f) p = 1.0f;
    g_traffic = t;
    g_peds = p;
    LOGI("world: traffic density x%.2f, pedestrians x%.2f (game:\\trafego.txt; 1.00 is the console)", t, p);
}

float F32(uint32_t va) {
    float f;
    uint32_t b = R32(va);
    memcpy(&f, &b, 4);
    return f;
}
uint32_t Bits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
bool Density(float d) { return _finite(d) && d >= 0.0f && d < 1000.0f; }

// the density argument at [esp+4] scaled for the original call, then put back
void ScaledCall(X86Context& ctx, uint8_t* base, void (*orig)(X86Context&, uint8_t*), float scale, const char* what) {
    const uint32_t arg = ctx.esp + 4;
    if (scale >= 1.0f || !mem::IsCommitted(arg) || !Density(F32(arg))) {
        orig(ctx, base);
        return;
    }
    const uint32_t bits = R32(arg);
    const float d = F32(arg);
    W32(arg, Bits(d * scale));
    static int told = 0;
    if (told < 6) {
        told++;
        LOGI("world: %s density %.3f -> %.3f", what, d, d * scale);
    }
    orig(ctx, base);
    W32(arg, bits);
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_002B9670);
X86_FUNC(sub_002B9670) {
    Load();
    ScaledCall(ctx, base, __imp__sub_002B9670, g_traffic, "road traffic");
}

X86_EXTERN_FUNC(__imp__sub_00222770);
X86_FUNC(sub_00222770) {
    Load();
    ScaledCall(ctx, base, __imp__sub_00222770, g_peds, "pedestrian");
}
