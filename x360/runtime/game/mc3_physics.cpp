// MC3 (2026-11-16): car physics in steps of at most 1/30 s while race time follows real time.
//
// The clock 0x20B6F0 (fixed step mode) copies the frame's step to the physics step 0x4B2D48 (clamped by 0x4B2D60 and
// 0x4B2D5C) and its inverse to 0x4B2D50, and the world update 0x1EDB0 runs the physics once a frame with it: the
// vehicle manager 0x2BDEC0, the physics simulator 0x29D210 ([0x558120]), and the vehicle manager again (its return
// address 0x1EEFC), which reaches each car's simulation (0xFEF80 -> 0x35DA80 -> 0x124FF0). Console, 2026-11-16: with
// the frame step at 1/15 s (frames under 30 fps, kept to real time by mc3_timing.cpp) the cars shook at rest; with
// passo.txt at 0.034 (one step of 1/30) they did not (video 06:09, frame differences at the countdown).
//
// So a frame longer than 1/30 s runs its physics N times, each of step / N (at most 1/30): mc3_timing.cpp sets
// 0x4B2D48 / 0x4B2D50 to the substep after the clock, the frame's own pass runs one, and the other N - 1 run here
// after it, as the game's own settling loop at the start of a race does (0x1D9E0: the simulator, then 0xFF320 for
// every car of the list [0x534074] and every one the manager [0x558814] has through 0x2BD920 / 0x2BF570). The frame
// time 0x4B2D70 (timers, animation, camera) keeps the whole step.
#include <string.h>

#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "host.h"

using kernel::R32;

int g_mc3_phys_substeps = 1;  // set by mc3_timing.cpp each frame
uint32_t g_mc3_phys_sub[2];   // the substep and its inverse (float bits), set with it

namespace {
const uint32_t kSimulator = 0x558120, kCarList = 0x534074, kAiCount = 0x53F2E0, kAiManager = 0x558814;
const uint32_t kSecondVehicleUpdate = 0x0001EEFC;
// MC3 (2026-11-17): after the finish (results screen) the world update is 0x1EBF0: clock, simulator, the vehicle
// manager once (return 0x1EC12). It got the substep but no extra steps: its physics ran at 1/N of real time
const uint32_t kOnlyVehicleUpdate = 0x0001EC12;

void CarStep(X86Context& ctx, uint32_t car) {
    if (!car || !mem::IsCommitted(car)) return;
    ctx.ecx = car;
    kernel::CallGuest(ctx, 0x000FF320);
}

// one more physics step, as 0x1D9F0..0x1DA6C
void ExtraStep(X86Context& ctx) {
    const uint32_t sim = R32(kSimulator);
    if (sim) {
        ctx.ecx = sim;
        kernel::CallGuest(ctx, 0x0029D210);
    }
    const uint32_t list = R32(kCarList);
    if (list && mem::IsCommitted(list)) {
        const int32_t n = int32_t(R32(list));
        for (int32_t i = 0; i < n && i < 64; i++) {
            const uint32_t entry = R32(list + 8) + uint32_t(i) * 0x43C;
            if (!R32(list + 8) || !mem::IsCommitted(entry + 0x20)) continue;
            CarStep(ctx, R32(entry + 0x20));
        }
    }
    const uint32_t ai = R32(kAiCount);
    if (ai && mem::IsCommitted(ai + 0x4C)) {
        const int32_t n = int32_t(R32(ai + 0x4C));
        for (int32_t i = 0; i < n && i < 64; i++) {
            ctx.ecx = R32(kAiManager);
            if (!ctx.ecx || !(kernel::CallGuest(ctx, 0x002BD920, uint32_t(i)) & 0xFF)) continue;
            const uint32_t o = kernel::CallGuest(ctx, 0x002BF570, uint32_t(i));
            if (o && mem::IsCommitted(o + 0x14)) CarStep(ctx, R32(o + 0x14));
        }
    }
}
}  // namespace

// the vehicle manager's update; the second call of the frame (0x1EEFC) ends the frame's physics
X86_EXTERN_FUNC(__imp__sub_002BDEC0);
// MC3 (2026-11-17): what the physics costs, every 10 s in the log: the frame's own pass (first vehicle manager call to
// the end of the second) and the extra substeps
static int64_t g_win_t0 = 0, g_win_ns = 0, g_extra_ns = 0, g_report_at = 0;
static uint32_t g_win_frames = 0, g_extra_steps = 0;
static uint32_t g_first_calls = 0, g_last_calls = 0;
static void PhysReport() {
    const int64_t now = host::NowNs();
    if (!g_report_at) g_report_at = now + 10000000000ll;
    if (now < g_report_at) return;
    if (g_win_frames)
        LOGI("physics cost: %u frames (first calls %u, last calls %u), last vehicle manager call %.2f ms a frame, %u extra steps %.2f ms a frame (%.2f ms each)", g_win_frames, g_first_calls, g_last_calls,
             double(g_win_ns) / 1e6 / g_win_frames, g_extra_steps, double(g_extra_ns) / 1e6 / g_win_frames,
             g_extra_steps ? double(g_extra_ns) / 1e6 / g_extra_steps : 0.0);
    g_win_ns = g_extra_ns = 0;
    g_win_frames = g_extra_steps = 0;
    g_first_calls = g_last_calls = 0;
    g_report_at = now + 10000000000ll;
}
// MC3 (2026-11-17, third way): the car's controls in the first call get the frame's whole time, the physics the
// substep. Console 2026-11-16 21:00 (release dt_so_na_fisica): with the substep from the first call on, the steering
// and throttle moved 1/N of what the car did in the N physics steps, and the car would not answer. Now the substep goes
// into 0x4B2D48 / 0x4B2D50 when the first call returns (0x1EE99), stays for the simulator and the second call (0x1EEFC)
// or the results screen's only call (0x1EC12, from its clock on), and the extra steps; the frame's own time comes back
// after them, for the rest of the game (the race clock). With the substep in the whole frame (release 2026-11-16 20:20)
// the race clock went at a third of real time.
static const uint32_t kFirstVehicleUpdate = 0x0001EE99;
static uint32_t g_whole[2];
static bool g_in_sub = false;
void Mc3PhysEnterSub() {
    if (g_in_sub || g_mc3_phys_substeps <= 1) return;
    g_whole[0] = R32(0x4B2D48);
    g_whole[1] = R32(0x4B2D50);
    kernel::W32(0x4B2D48, g_mc3_phys_sub[0]);
    kernel::W32(0x4B2D50, g_mc3_phys_sub[1]);
    g_in_sub = true;
}
static void LeaveSub() {
    if (!g_in_sub) return;
    kernel::W32(0x4B2D48, g_whole[0]);
    kernel::W32(0x4B2D50, g_whole[1]);
    g_in_sub = false;
}
X86_FUNC(sub_002BDEC0) {
    const uint32_t ret = R32(ctx.esp);
    const bool last = ret == kSecondVehicleUpdate || ret == kOnlyVehicleUpdate;
    if (ret == kFirstVehicleUpdate) g_first_calls++;
    if (last) {
        g_last_calls++;
        g_win_t0 = host::NowNs();
    }
    __imp__sub_002BDEC0(ctx, base);
    if (ret == kFirstVehicleUpdate) {
        Mc3PhysEnterSub();  // the controls had the whole frame; the simulator and the second call get the substep
        return;
    }
    if (!last) return;
    g_win_ns += host::NowNs() - g_win_t0;
    g_win_frames++;
    const int extra = g_mc3_phys_substeps - 1;
    g_mc3_phys_substeps = 1;
    if (extra > 0) {
        const uint32_t eax = ctx.eax;
        const bool entered = g_in_sub;
        if (!entered) {  // no first call this frame: the extra steps still take the substep
            g_mc3_phys_substeps = extra + 1;
            Mc3PhysEnterSub();
            g_mc3_phys_substeps = 1;
        }
        const int64_t te = host::NowNs();
        for (int i = 0; i < extra; i++) ExtraStep(ctx);
        g_extra_ns += host::NowNs() - te;
        g_extra_steps += uint32_t(extra);
        ctx.eax = eax;
        static int told = 0;
        if (told < 4) {
            told++;
            float f;
            memcpy(&f, &g_mc3_phys_sub[0], 4);
            LOGI("physics: %d extra step(s) of %.4f s after the frame's own (substep from the first call: %d)", extra, double(f), int(entered));
        }
    }
    LeaveSub();
    PhysReport();
}
