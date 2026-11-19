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
#include <stdio.h>
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

// MC3 (2026-11-18): the garage's settling loops. When a car is put on show (bought, picked, painted, the garage
// entered), the game drops it onto the floor by running the simulator and that car's step 60 times in a row
// (0x14E090, 0x14E200, 0x151664, 0x152FC0, 0x1CFC10: "mov edi, 0x3C", 0x29D210 then 0xFF320, dec, jnz). On the console
// the physics step 0x4B2D48 was the fixed 1/60 there; here it holds the last frame's time (up to 0.1 s, the garage
// runs at 15 to 20 fps on the console): 60 steps of 0.1 s left the car sunk into the floor and smoking (console video
// of 2026-11-18 18:50). So these calls get the game's own step, and the step goes back after each.
extern float g_mc3_original_step;  // game/mc3_timing.cpp: 1 / (rate * substeps) the game asked for
namespace {
bool SettleRet(uint32_t r) {
    switch (r) {
    case 0x0014E09B: case 0x0014E20B: case 0x0015166F: case 0x00152FCB: case 0x001CFC1B:  // simulator
    case 0x0014E0AD: case 0x0014E21D: case 0x00151681: case 0x00152FDD: case 0x001CFC3C:  // the car's step
        return true;
    }
    return false;
}
uint32_t g_settle_calls = 0;
float g_settle_was = 0;
struct FixedStep {
    uint32_t a, b;
    bool on;
    explicit FixedStep(uint32_t ret) : a(0), b(0), on(false) {
        if (!SettleRet(ret)) return;
        const float step = g_mc3_original_step > 0 && g_mc3_original_step < 0.05f ? g_mc3_original_step : 1.0f / 60;
        a = R32(0x4B2D48);
        b = R32(0x4B2D50);
        memcpy(&g_settle_was, &a, 4);
        uint32_t s, i;
        const float inv = 1.0f / step;
        memcpy(&s, &step, 4);
        memcpy(&i, &inv, 4);
        kernel::W32(0x4B2D48, s);
        kernel::W32(0x4B2D50, i);
        on = true;
        g_settle_calls++;
    }
    ~FixedStep() {
        if (!on) return;
        kernel::W32(0x4B2D48, a);
        kernel::W32(0x4B2D50, b);
    }
};
}  // namespace
// MC3 (2026-11-18): who calls the clock, the simulator, a car's step and the vehicle manager, every 10 s in the log
struct RetHist {
    const char* name;
    uint32_t ret[12], n[12];
    void Add(uint32_t r) {
        for (int i = 0; i < 12; i++) {
            if (n[i] && ret[i] == r) { n[i]++; return; }
            if (!n[i]) { ret[i] = r; n[i] = 1; return; }
        }
    }
    void Dump() {
        char b[400];
        int o = 0;
        for (int i = 0; i < 12 && n[i] && o < 360; i++) o += sprintf(b + o, " %X:%u", ret[i], n[i]);
        if (o) LOGI("calls: %s from%s", name, b);
        memset(ret, 0, sizeof(ret));
        memset(n, 0, sizeof(n));
    }
};
RetHist g_h_clock = {"clock"}, g_h_sim = {"simulator"}, g_h_car = {"car step"}, g_h_vm = {"vehicle manager"};
static void HistReport();
void Mc3CountClock(uint32_t r) {
    g_h_clock.Add(r);
    HistReport();
}
static void HistReport() {
    static int64_t at = 0;
    const int64_t now = host::NowNs();
    if (!at) at = now + 10000000000ll;
    if (now < at) return;
    at = now + 10000000000ll;
    g_h_clock.Dump();
    g_h_sim.Dump();
    g_h_car.Dump();
    g_h_vm.Dump();
    if (g_settle_calls) {
        LOGI("physics: %u settling calls on the game's own step (the step there was %.4f s)", g_settle_calls, double(g_settle_was));
        g_settle_calls = 0;
    }
}
X86_EXTERN_FUNC(__imp__sub_0029D210);
X86_FUNC(sub_0029D210) {
    g_h_sim.Add(R32(ctx.esp));
    FixedStep f(R32(ctx.esp));
    __imp__sub_0029D210(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_000FF320);
X86_FUNC(sub_000FF320) {
    g_h_car.Add(R32(ctx.esp));
    FixedStep f(R32(ctx.esp));
    __imp__sub_000FF320(ctx, base);
    HistReport();
}

// MC3 (2026-11-18): one more step of the garage's and the results screen's world update (0x1EBF0): the simulator, then
// the vehicle manager, as 0x1EBFC..0x1EC12. The garage's car is stepped by the vehicle manager (0xFEF80 -> 0x35DA80),
// not by 0xFF320 (none in the garage, console log 2026-11-18 20:23, "calls:"): with ExtraStep the simulator ran 3 times
// a frame and the car's own forces once, and the cars on show sank into the floor and shook. The manager's hook below
// sees these calls come back to 0xFFFFFFF0, not a frame's own, and passes them through.
static void ExtraStepWithManager(X86Context& ctx) {
    const uint32_t sim = R32(kSimulator);
    if (sim) {
        ctx.ecx = sim;
        kernel::CallGuest(ctx, 0x0029D210);
    }
    const uint32_t vm = R32(kAiManager);
    if (vm) {
        ctx.ecx = vm;
        kernel::CallGuest(ctx, 0x002BDEC0);
    }
}

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
    g_h_vm.Add(ret);
    HistReport();
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
        for (int i = 0; i < extra; i++) {
            if (ret == kOnlyVehicleUpdate)
                ExtraStepWithManager(ctx);
            else
                ExtraStep(ctx);
        }
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
