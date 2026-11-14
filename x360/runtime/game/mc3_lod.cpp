// MC3 (2026-11-13): level of detail and prop draw distance scale, from the PC reference
// (port/templates/game/src/game/lod.cpp and docs/notes/lod_fov.md, same project; adapted to VS2010 and this runtime).
// Here it goes the other way: a scale under 1 brings the model LOD switches and the city props' draw distance closer,
// so a frame draws and walks less. The loop profiler (2026-11-13, Xenia race) put the prop manager's draw
// sub_0019BF50 among the title thread's heaviest, and each draw costs the title thread 15 to 20 us here.
//   game:\lod.txt holds the scale (0.4 to 1.0); without it, kDefaultScale. 1.0 is the console.
// What the scale changes (the PC file has the evidence):
//  1. Model LOD switches (cars and traffic, peds, entities, fx): the type's LOD selector sub_000E62B0 asks the viewport
//     sphere test sub_002D45C0 for the bounding sphere's depth; that one caller gets radius + (depth - radius) / scale,
//     the same as every switch distance times the scale. The lodgroup prefetch sub_000E9260 gets distance / scale.
//  2. City props (cull_dist 100 to 400 m): prop type +0x9C, set by its loader sub_0019D980, and the prop manager's
//     search range +0x303C, both read by the prop manager's draw sub_0019BF50.
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>

#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "host.h"

#include <xtl.h>

using kernel::R32;
using kernel::W32;

namespace {
const float kDefaultScale = 0.75f;
const uint32_t kTypeLodReturn = 0x000E6342;  // return address of sub_000E62B0's call to sub_002D45C0
const uint32_t kPropCull = 0x9C, kPropRadius = 0xA0, kPropFlags = 0x94;
const uint32_t kPropManagerRange = 0x303C;

float g_scale = -1;

float Scale() {
    if (g_scale < 0) {
        float s = kDefaultScale;
        if (FILE* f = fopen("game:\\lod.txt", "rb")) {
            char b[32] = {0};
            fread(b, 1, sizeof(b) - 1, f);
            fclose(f);
            double v = strtod(b, NULL);
            if (v > 0) s = float(v);
        }
        if (!_finite(s)) s = 1.0f;
        if (s < 0.4f) s = 0.4f;
        if (s > 1.0f) s = 1.0f;
        g_scale = s;
        LOGI("lod: scale %.2f (game:\\lod.txt; 1.00 is the console)", s);
    }
    return g_scale;
}

float F32(uint32_t va) {
    float f;
    uint32_t b = R32(va);
    memcpy(&f, &b, 4);
    return f;
}
void WF32(uint32_t va, float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    W32(va, b);
}
bool Committed(uint32_t va, uint32_t bytes) {
    const uint64_t end = uint64_t(va) + bytes;
    if (!bytes || va < 0x10000 || end > (uint64_t(1) << 32)) return false;
    for (uint64_t page = va & ~uint64_t(mem::kPage - 1); page < end; page += mem::kPage)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}
bool Plausible(uint32_t va) { return va >= 0x10000 && va < 0x84000000 && (va & 3) == 0; }

struct Prop {
    float original, written;
    uint32_t radius, flags;
};
host::Mutex g_lock;
std::map<uint32_t, Prop> g_props;  // prop type -> what it loaded with and what was written

bool PropReadable(uint32_t type) { return Plausible(type) && Committed(type + kPropFlags, 0x10); }

void ApplyProps(float s) {
    g_lock.lock();
    for (std::map<uint32_t, Prop>::iterator it = g_props.begin(); it != g_props.end();) {
        const uint32_t type = it->first;
        Prop& p = it->second;
        if (!PropReadable(type) || R32(type + kPropRadius) != p.radius || R32(type + kPropFlags) != p.flags ||
            F32(type + kPropCull) != p.written) {
            g_props.erase(it++);  // freed, reused or reloaded: the loader hook tracks it again
            continue;
        }
        const float want = p.original * s;
        if (_finite(want) && want != p.written) {
            WF32(type + kPropCull, want);
            p.written = want;
        }
        ++it;
    }
    g_lock.unlock();
}
}  // namespace

// viewport sphere test (thiscall; x, y, z, radius, float* depth_out): only the type LOD selector's call is changed
X86_EXTERN_FUNC(__imp__sub_002D45C0);
X86_FUNC(sub_002D45C0) {
    if (R32(ctx.esp) != kTypeLodReturn) {
        __imp__sub_002D45C0(ctx, base);
        return;
    }
    const float radius = F32(ctx.esp + 0x10);
    const uint32_t out = R32(ctx.esp + 0x14);
    __imp__sub_002D45C0(ctx, base);
    const float s = Scale();
    if (s == 1.0f || !out || !Plausible(out) || !Committed(out, 4) || !_finite(radius)) return;
    const float depth = F32(out);
    if (!_finite(depth)) return;
    WF32(out, radius + (depth - radius) / s);
}

// lodgroup mesh prefetch (thiscall; x, distance)
X86_EXTERN_FUNC(__imp__sub_000E9260);
X86_FUNC(sub_000E9260) {
    const float s = Scale();
    if (s != 1.0f) {
        const float d = F32(ctx.esp + 8);
        if (_finite(d)) WF32(ctx.esp + 8, d / s);
    }
    __imp__sub_000E9260(ctx, base);
}

// prop type loader (virtual, thiscall): cull_dist is final when it returns
X86_EXTERN_FUNC(__imp__sub_0019D980);
X86_FUNC(sub_0019D980) {
    const uint32_t type = ctx.ecx;
    __imp__sub_0019D980(ctx, base);
    if (!PropReadable(type)) return;
    const float v = F32(type + kPropCull);
    if (!_finite(v) || v <= 0.0f) return;
    g_lock.lock();
    std::map<uint32_t, Prop>::iterator it = g_props.find(type);
    if (it == g_props.end() || it->second.written != v) {
        Prop p;
        p.original = p.written = v;
        p.radius = R32(type + kPropRadius);
        p.flags = R32(type + kPropFlags);
        g_props[type] = p;
    }
    g_lock.unlock();
}

// prop manager draw (virtual, thiscall; once per view and frame): prop types from the scale, the search range scaled
// for this call only
X86_EXTERN_FUNC(__imp__sub_0019BF50);
X86_FUNC(sub_0019BF50) {
    const float s = Scale();
    ApplyProps(s);
    const uint32_t range = ctx.ecx + kPropManagerRange;
    float original = 0.0f, written = 0.0f;
    bool scaled = false;
    if (s != 1.0f && Plausible(ctx.ecx) && Committed(range, 4)) {
        original = F32(range);
        written = original * s;
        if (_finite(original) && original > 0.0f && _finite(written)) {
            WF32(range, written);
            scaled = true;
        }
    }
    __imp__sub_0019BF50(ctx, base);
    if (scaled && Committed(range, 4) && F32(range) == written) WF32(range, original);
}

// prop manager destructor (thiscall): its prop types go with it
X86_EXTERN_FUNC(__imp__sub_0019A630);
X86_FUNC(sub_0019A630) {
    g_lock.lock();
    g_props.clear();
    g_lock.unlock();
    __imp__sub_0019A630(ctx, base);
}
