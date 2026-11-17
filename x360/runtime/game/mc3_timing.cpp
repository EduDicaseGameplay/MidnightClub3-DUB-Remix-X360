// MC3 (2026-11-12): race time kept to real time while the frame rate is under the console's 30.
//
// The engine's clock (time manager 0x20B6F0, once a frame) runs in fixed step mode in every race: 0x20BB00(rate,
// substeps), called when play starts, sets [0x534770] and keeps the step 1 / (rate * substeps) at 0x4B2D58; each frame
// then advances exactly that, whatever time passed. The console ran races at 30 fps, so each frame was 1/30 s. Here a
// race frame takes 55 to 110 ms (console test 20, 2026-11-12: 9 to 16 fps in San Diego), and the cars, the traffic and
// the race clock ran at half speed or less: the game looked slower than its frame rate.
//
// Same fix as the PC reference (port/templates/game/src/game/hooks.cpp sub_0020B6F0, docs/notes/replays.md): a late
// frame advances as many of the game's own steps as the time that passed, rounded, with the remainder carried to the
// next frame. At most 1/15 s a frame (the PC port's largest step, 4 fields of 1/60, ran there without trouble; the
// game asks for rate 60, substeps 1, so up to 4 of its steps); a load or a pause (over 0.25 s) advances one, as before. A replay re-simulates the race
// from the recorded input and does not record the step, so the step of every recorded frame is kept here and forced
// again on the same frame of the playback (the PC's replay.cpp, its first part).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "host.h"

#include <xtl.h>

using kernel::R32;
using kernel::W32;

extern int g_mc3_phys_substeps;  // game/mc3_physics.cpp
extern uint32_t g_mc3_phys_sub[2];
void Mc3PhysEnterSub();

namespace {
const uint32_t kPrevTick = 0x534778;   // time stamp low dword at the last update (733.33 MHz)
const uint32_t kFixedStep = 0x534770;  // dword: fixed step mode
const uint32_t kStep = 0x4B2D58;       // float: seconds a fixed step frame advances
const uint32_t kFrameTime = 0x4B2D70;  // float: seconds the update settled on
const uint32_t kStream = 0x534AC4;     // dword: the open replay record/playback stream (0 = none)
const uint32_t kPlaying = 0x534A9C;    // byte: 1 = playing back, 0 = recording
const uint32_t kRecorded = 0x534AD4;   // dword: frames recorded
const uint32_t kPlayed = 0x534AD8;     // dword: frames played back
const double kTicksPerSecond = 733333333.0;
// the most a frame advances. The PC port's largest is 1/15 (4 fields of 1/60); here a race frame takes up to 0.12 s
// on the console, and with 1/15 the race ran at 0.55 of real time there (log of 2026-11-16, "x0.55"): visibly slow.
// With the physics in substeps of at most 1/30 (game/mc3_physics.cpp) a longer frame no longer shakes the cars, so
// 0.1 s (3 substeps): the race keeps closer to real time for one more physics step in the slowest frames.
const double kMaxFrameStep = 0.1 + 1e-4;
const size_t kMaxReplayFrames = 1u << 20;

float g_original_step = 0;  // what the game asked for, 1 / (rate * substeps)
double g_carry = 0;
std::vector<uint32_t> g_replay_steps;
// game:\passo.txt: the most a frame advances, in seconds (cars shook at rest on the console with 1/15, test of
// 2026-11-16: a hypothesis to check with 0.034, one step of 1/30)
// game:\subpasso.txt: the largest physics substep in seconds, 0.034 to 0.067 (without it 1/30, the console's step:
// with 1/15 the cars shook at rest). A larger one runs fewer extra physics steps in a slow frame (MC3, 2026-11-17)
double MaxSubstep() {
    static double v = -1;
    if (v < 0) {
        v = 1.0 / 30.0 + 1e-4;
        if (FILE* f = fopen("game:\\subpasso.txt", "rb")) {
            char b[32] = {0};
            fread(b, 1, sizeof(b) - 1, f);
            fclose(f);
            double x = strtod(b, NULL);
            if (x >= 0.034 && x <= 0.067) v = x + 1e-4;
        }
        LOGI("timing: the largest physics substep is %.4f s (game:\\subpasso.txt)", v);
    }
    return v;
}
double MaxFrameStep() {
    static double v = -1;
    if (v < 0) {
        v = kMaxFrameStep;
        if (FILE* f = fopen("game:\\passo.txt", "rb")) {
            char b[32] = {0};
            fread(b, 1, sizeof(b) - 1, f);
            fclose(f);
            double x = strtod(b, NULL);
            if (x >= 0.034 && x <= 0.134) v = x;
        }
        LOGI("timing: the most a frame advances is %.4f s (game:\\passo.txt)", v);
    }
    return v;
}  // by recorded frame: the step bits it ran with
// every 10 s in the log: race frames, game seconds against real seconds
double g_real = 0, g_game = 0, g_since = 0;
uint32_t g_frames = 0, g_late = 0, g_waits = 0;

uint32_t Bits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
float Float(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}

// fixed step only, after the step for this frame is chosen: a recording keeps it, a playback forces the recorded one
bool ReplayStep() {
    if (!R32(kStream)) return false;
    if (!kernel::G8(kPlaying)) {
        const uint32_t frame = R32(kRecorded);
        if (frame < kMaxReplayFrames) {
            if (frame >= g_replay_steps.size()) g_replay_steps.resize(size_t(frame) + 1, 0);
            g_replay_steps[frame] = R32(kStep);
        }
        return false;
    }
    const uint32_t frame = R32(kPlayed);
    uint32_t bits = frame < g_replay_steps.size() ? g_replay_steps[frame] : 0;
    if (!bits && g_original_step > 0) bits = Bits(g_original_step);
    if (bits) W32(kStep, bits);
    return bits != 0;
}
}  // namespace

// MC3 (2026-11-13), the rest of the PC reference's replay.cpp (port/templates/game/src/game, same project): a replay is
// the race simulated again from the recorded pad input, checked every 30 frames against the recorded player position
// (0x2DAC0); the first mismatch flags the recorder (+0x14) and the next frame ends the replay. With the step varying
// from frame to frame the re-simulation drifts by float rounding, so: the step is kept per frame (ReplayStep), the
// player car's transform at the end of each recorded frame is written back on the same played frame, and a failed
// check is cleared so the replay plays on. Not tested here yet (no Xenia or console run has opened a replay).
namespace {
const uint32_t kRecDiverged = 0x14;
struct CarTransform {
    uint32_t size;
    uint32_t words[16];
};
std::vector<CarTransform> g_transforms;  // by recorded frame
bool g_snap_told = false, g_drift_told = false;

bool Unit(uint32_t a) {
    float l = 0;
    for (int i = 0; i < 3; i++) {
        float v = Float(R32(a + 4 * i));
        l += v * v;
    }
    return l > 0.998f && l < 1.002f;
}
// the player car transform: a Matrix34 (translation at +0x80) or a 4x4 with 16 byte rows; size 0 if neither
uint32_t TransformSpan(uint32_t p, uint32_t& start) {
    if (!mem::IsCommitted(p + 0x50) || !mem::IsCommitted(p + 0x8F)) return 0;
    if (Unit(p + 0x5C) && Unit(p + 0x68) && Unit(p + 0x74)) { start = p + 0x5C; return 48; }
    if (Unit(p + 0x50) && Unit(p + 0x60) && Unit(p + 0x70)) { start = p + 0x50; return 64; }
    return 0;
}
// player index (0x2694D0) -> object (0x2BF570) -> +0x14 -> +8 -> +0x68 -> +0x10: the matrix holding the position
bool PlayerMatrix(X86Context& ctx, uint32_t& matrix) {
    const X86Context saved = ctx;
    const uint32_t index = kernel::CallGuest(ctx, 0x2694D0);
    const uint32_t object = kernel::CallGuest(ctx, 0x2BF570, index);
    ctx = saved;
    static const uint32_t offs[4] = {0x14u, 0x8u, 0x68u, 0x10u};
    uint32_t p = object;
    for (int i = 0; i < 4; i++) {
        if (!p || !mem::IsCommitted(p + offs[i])) return false;
        p = R32(p + offs[i]);
    }
    if (!p || !mem::IsCommitted(p + 0x88)) return false;
    matrix = p;
    return true;
}
// a stream opening starts the simulated race: recording and playback start from the game's own step
void CanonicalStart(const char* what) {
    if (g_original_step <= 0) return;
    if (R32(kStep)) W32(kStep, Bits(g_original_step));
    W32(kFrameTime, Bits(g_original_step));
    g_carry = 0;
    LOGI("replay: %s opened, starting from a step of %.5f s", what, g_original_step);
}
}  // namespace

// open the stream for recording (cdecl, stream, al = opened)
X86_EXTERN_FUNC(__imp__sub_0020EA20);
X86_FUNC(sub_0020EA20) {
    __imp__sub_0020EA20(ctx, base);
    if ((ctx.eax & 0xFF) && R32(kStream)) {
        g_replay_steps.clear();
        g_transforms.clear();
        CanonicalStart("recording");
    }
}

// open the stream for playback (cdecl, stream, al = opened): Instant Replay and Restart Replay
X86_EXTERN_FUNC(__imp__sub_0020EAC0);
X86_FUNC(sub_0020EAC0) {
    __imp__sub_0020EAC0(ctx, base);
    if (!(ctx.eax & 0xFF) || !R32(kStream)) {
        LOGI("replay: the recording could not be opened for playback");
        return;
    }
    CanonicalStart("playback");
    LOGI("replay: playing %u frames, steps kept for %u, transforms for %u", R32(kRecorded), unsigned(g_replay_steps.size()),
         unsigned(g_transforms.size()));
    g_snap_told = g_drift_told = false;
}

// end of a record/playback frame with the position check every 30 frames (thiscall, recorder, al)
X86_EXTERN_FUNC(__imp__sub_0002DAC0);
X86_FUNC(sub_0002DAC0) {
    const uint32_t rec = ctx.ecx;
    const bool playing = R32(kStream) && kernel::G8(kPlaying);
    const bool was = kernel::G8(rec + kRecDiverged) != 0;
    if (R32(kStream)) {
        const uint32_t frame = playing ? R32(kPlayed) : R32(kRecorded);
        uint32_t matrix = 0, start = 0;
        if (frame < kMaxReplayFrames && PlayerMatrix(ctx, matrix)) {
            const uint32_t size = TransformSpan(matrix, start);
            if (!playing && size) {
                if (frame >= g_transforms.size()) {
                    CarTransform empty;
                    memset(&empty, 0, sizeof(empty));
                    g_transforms.resize(size_t(frame) + 1, empty);
                }
                CarTransform& t = g_transforms[frame];
                t.size = size;
                for (uint32_t i = 0; i < size / 4; i++) t.words[i] = R32(start + 4 * i);
            } else if (playing && size && frame < g_transforms.size() && g_transforms[frame].size == size) {
                const CarTransform& t = g_transforms[frame];
                for (uint32_t i = 0; i < t.size / 4; i++) W32(start + 4 * i, t.words[i]);
                if (!g_snap_told) {
                    g_snap_told = true;
                    LOGI("replay: keeping the player car on its recorded path (%s transform)", t.size == 48 ? "3x4" : "4x4");
                }
            }
        }
    }
    __imp__sub_0002DAC0(ctx, base);
    if (!playing) return;
    if (!was && kernel::G8(rec + kRecDiverged)) {
        kernel::G8(rec + kRecDiverged) = 0;
        if (!g_drift_told) {
            g_drift_told = true;
            LOGI("replay: playback drifted from the recording at frame %u of %u (position check); playing on", R32(kPlayed),
                 R32(kRecorded));
        }
    }
}

// 0x20BB00(float rate, int substeps), cdecl: fixed step mode
X86_EXTERN_FUNC(__imp__sub_0020BB00);
X86_FUNC(sub_0020BB00) {
    const float rate = Float(R32(ctx.esp + 4));
    const uint32_t substeps = R32(ctx.esp + 8);
    __imp__sub_0020BB00(ctx, base);
    g_original_step = Float(R32(kStep));
    g_carry = 0;
    LOGI("timing: the game asked for a fixed step, rate %.2f, substeps %u: step %.5f s", rate, substeps, g_original_step);
}

// 0x20B6F0, the clock, once a frame
int SlowMs();
X86_EXTERN_FUNC(__imp__sub_0001EDB0);
namespace {

// MC3 (2026-11-17): the race in whole steps of the console's own, as many as real time asks, and one picture. The main
// loop 0x1F9D0 already runs a second update after the picture when its flag asks (0x1FC20: the pad, 0x23E30, the
// state's pending call, the update 0x1EDB0, 0x23D00); here the update called at 0x1FB76 runs again in the same way
// until the race has caught up with real time, every one of them with the step of 1/30 the game asked for. The cars,
// the controls, the timers and the race clock then see exactly the console's step, as on the original at 30 fps,
// and only the picture is drawn less often. A frame owes at most MaxUpdates() steps (game:\atualizacoes.txt, 1 to 6,
// default 3); time past that is dropped, and the race runs slower only then.
bool g_forced = false;           // the clock keeps the game's own step (an update of the loop below)
int64_t g_upd_ns = 0, g_frame_ns = 0, g_upd_frames = 0, g_upd_count = 0, g_upd_dropped = 0, g_upd_report = 0;
int64_t g_last_frame = 0;
double g_acc = 0;
int MaxUpdates() {
    static int v = -1;
    if (v < 0) {
        v = 0;
        if (FILE* f = fopen("game:\atualizacoes.txt", "rb")) {
            char b[16] = {0};
            fread(b, 1, sizeof(b) - 1, f);
            fclose(f);
            const int x = atoi(b);
            if (x >= 0 && x <= 6) v = x;
        }
        LOGI("timing: up to %d updates of the game's own step a picture (game:\atualizacoes.txt; 0 = the variable step)", v);
    }
    return v;
}
void RunUpdate(X86Context& ctx, uint8_t* base, uint32_t self) {
    ctx.ecx = self;
    const int64_t t = host::NowNs();
    g_forced = true;
    __imp__sub_0001EDB0(ctx, base);
    g_forced = false;
    g_upd_ns += host::NowNs() - t;
    g_upd_count++;
}
// what 0x1FC20..0x1FC7D does around a second update, before it (pad, 0x23E30, the state's pending call)
void BeforeUpdate(X86Context& ctx, uint32_t self) {
    const uint32_t pad = R32(0x557EEC);
    if (pad) {
        ctx.ecx = pad;
        kernel::CallGuest(ctx, R32(R32(pad) + 8));
    }
    uint32_t st = R32(0x557ED0);
    if (st && R32(st + 4) != 6 && R32(0x4CBAE0)) {
        ctx.ecx = R32(0x4CBAE0);
        kernel::CallGuest(ctx, 0x00023E30);
        st = R32(0x557ED0);
    }
    if (*static_cast<volatile uint8_t*>(kernel::HostPtr(self + 0x24))) {
        ctx.ecx = st;
        kernel::CallGuest(ctx, R32(R32(st) + 4), 1u);
        *static_cast<volatile uint8_t*>(kernel::HostPtr(self + 0x24)) = 0;
    }
}
// and after it (0x1FC61..0x1FC7D)
void AfterUpdate(X86Context& ctx) {
    const uint32_t st = R32(0x557ED0);
    const uint32_t s = st ? R32(st + 4) : 0;
    if ((s == 2 || s == 3) && R32(0x4CBAE0)) {
        ctx.ecx = R32(0x4CBAE0);
        kernel::CallGuest(ctx, 0x00023D00);
    }
}
}  // namespace

X86_FUNC(sub_0001EDB0) {
    const bool main_update = R32(ctx.esp) == 0x0001FB7B;
    const bool fixed = R32(kFixedStep) != 0 && g_original_step > 0 && g_original_step < 0.2f;
    if (!main_update || !fixed || MaxUpdates() == 0) {
        __imp__sub_0001EDB0(ctx, base);
        return;
    }
    if (SlowMs()) {
        kernel::GuestUnlock unlocked;
        Sleep(DWORD(SlowMs()));
    }
    const double step = g_original_step;
    int64_t now = host::NowNs();
    double since = g_last_frame ? double(now - g_last_frame) / 1e9 : step;
    if (since > 0.25) {  // a load or a pause: one step, as the console
        since = step;
        g_acc = 0;
    }
    g_acc += since;
    // a picture early for its step (over 30 fps) waits for it, the guest lock let go
    if (g_acc < step) {
        const int64_t end = now + int64_t((step - g_acc) * 1e9);
        {
            kernel::GuestUnlock unlocked;
            while (end - host::NowNs() > 2000000) Sleep(1);
            while (host::NowNs() < end) {
            }
        }
        const int64_t t = host::NowNs();
        g_acc += double(t - now) / 1e9;
        now = t;
    }
    g_last_frame = now;
    int n = int(g_acc / step + 1e-6);
    if (n < 1) n = 1;
    if (n > MaxUpdates()) {
        g_upd_dropped += n - MaxUpdates();
        n = MaxUpdates();
        g_acc = step * n;  // the rest is dropped
    }
    g_acc -= step * n;
    if (g_acc < 0) g_acc = 0;
    const uint32_t self = ctx.ecx;
    const uint32_t keep_ebx = ctx.ebx, keep_ebp = ctx.ebp, keep_esi = ctx.esi, keep_edi = ctx.edi, keep_esp = ctx.esp;
    RunUpdate(ctx, base, self);
    for (int i = 1; i < n; i++) {
        AfterUpdate(ctx);
        BeforeUpdate(ctx, self);
        RunUpdate(ctx, base, self);
    }
    ctx.ebx = keep_ebx;
    ctx.ebp = keep_ebp;
    ctx.esi = keep_esi;
    ctx.edi = keep_edi;
    ctx.esp = keep_esp;
    g_upd_frames++;
    // every 10 s: pictures, updates, what they cost, and the race time against real time
    if (!g_upd_report) g_upd_report = now;
    if (now - g_upd_report >= 10000000000ll) {
        const double secs = double(now - g_upd_report) / 1e9;
        LOGI("timing: %lld pictures, %lld updates of %.4f s (%.2f a picture, %lld dropped), an update %.2f ms; race %.2f s over "
             "%.2f real s (x%.2f)",
             g_upd_frames, g_upd_count, step, double(g_upd_count) / double(g_upd_frames ? g_upd_frames : 1), g_upd_dropped,
             g_upd_count ? double(g_upd_ns) / 1e6 / double(g_upd_count) : 0.0, double(g_upd_count) * step, secs,
             double(g_upd_count) * step / secs);
        g_upd_frames = g_upd_count = g_upd_dropped = 0;
        g_upd_ns = 0;
        g_upd_report = now;
    }
}
X86_EXTERN_FUNC(__imp__sub_0020B6F0);
// game:\lento.txt: test only, ms each race frame is held (Xenia runs races near 30 fps, the console at 8 to 12)
int SlowMs() {
    static int v = -1;
    if (v < 0) {
        v = 0;
        if (FILE* f = fopen("game:\\lento.txt", "rb")) {
            char b[16] = {0};
            fread(b, 1, sizeof(b) - 1, f);
            fclose(f);
            v = atoi(b);
            if (v < 0 || v > 200) v = 0;
        }
        if (v) LOGW("timing: game:\\lento.txt, every race frame held %d ms (test)", v);
    }
    return v;
}

X86_FUNC(sub_0020B6F0) {
    if (g_forced) {  // an update of the race loop above: the game's own step, as the console
        W32(kStep, Bits(g_original_step));
        g_carry = 0;
        ReplayStep();
        g_mc3_phys_substeps = 1;
        __imp__sub_0020B6F0(ctx, base);
        g_mc3_phys_substeps = 1;
        return;
    }
    if (R32(kFixedStep) && SlowMs() && MaxUpdates() == 0) {
        kernel::GuestUnlock unlocked;
        Sleep(DWORD(SlowMs()));
    }
    const uint32_t clock_ret = R32(ctx.esp);
    uint32_t now = uint32_t(x86_rdtsc());
    double measured = double(uint32_t(now - R32(kPrevTick))) / kTicksPerSecond;
    const bool fixed = R32(kFixedStep) != 0 && g_original_step > 0 && g_original_step < 0.2f;
    if (fixed) {
        const double step = g_original_step;
        // MC3 (2026-11-16): the most a frame advances, game:\passo.txt (seconds, 0.034 to 0.1; without it 1/15)
        const double max_adv = MaxFrameStep() > step ? MaxFrameStep() : step;
        uint32_t n = 1;
        if (measured < 0.25) {  // longer is a load or a pause (4.5 steps of 1/60 would be 75 ms, a slow frame here)
            // a frame early for its step (a light scene, over 30 fps in a race) waits for the step's time, or the race
            // runs ahead of real time (Xenia, 2026-11-12: x1.20 with the car against a wall). Without the lock of the
            // game's threads, sleeping to 2 ms before and spinning the rest (JSRF-23, game/jsrf.cpp Pace)
            if (measured + g_carry < step) {
                const int64_t end = host::NowNs() + int64_t((step - measured - g_carry) * 1e9);
                {
                    kernel::GuestUnlock unlocked;
                    while (end - host::NowNs() > 2000000) Sleep(1);
                    while (host::NowNs() < end) {
                    }
                }
                now = uint32_t(x86_rdtsc());
                measured = double(uint32_t(now - R32(kPrevTick))) / kTicksPerSecond;
                g_waits++;
            }
            // MC3 (2026-11-15): the frame advances the time it took, not a whole number of steps. Whole steps of 1/30
            // alternated 1 and 2 on the console at 14 to 17 fps (126 of 141 frames advanced two), so a car moved 33 ms
            // on one frame and 66 ms on the next with both frames about as long: the cars shook on the screen (not in
            // Xenia, near 30 fps, one step a frame). Between one step and the largest (1/15 s); what is left over a
            // longer frame is carried to the next, as before.
            const double want = measured + g_carry;
            double adv = want;
            if (adv < step) adv = step;
            if (adv > max_adv) adv = max_adv;
            g_carry = want - adv;
            if (g_carry > step) g_carry = step;
            if (g_carry < -step) g_carry = -step;
            W32(kStep, Bits(float(adv)));
            n = adv > step * 1.01 ? 2 : 1;
        } else {
            g_carry = 0;  // a load or a pause: one step, as the console's
            W32(kStep, Bits(float(step)));
        }
        if (ReplayStep()) g_carry = 0;
        if (n > 1) g_late++;
    } else {
        g_carry = 0;
    }
    __imp__sub_0020B6F0(ctx, base);
    // MC3 (2026-11-16): the physics in substeps of at most 1/30 s (game/mc3_physics.cpp): the clock just set the
    // physics step 0x4B2D48 / 0x4B2D50 from the frame time; a longer frame gets N of step / N
    g_mc3_phys_substeps = 1;
    if (fixed && measured < 0.25) {
        const float frame = Float(R32(kFrameTime));
        const int n = frame > 0 ? int(ceil(double(frame) / MaxSubstep())) : 1;
        if (n > 1 && n <= 8) {
            const float sub = frame / float(n);
            g_mc3_phys_sub[0] = Bits(sub);  // into 0x4B2D48 / 0x4B2D50 for the physics only (game/mc3_physics.cpp)
            g_mc3_phys_sub[1] = Bits(1.0f / sub);
            g_mc3_phys_substeps = n;
            // the race's world update (0x1EDB0, clock returns to 0x1EE55) skips its first vehicle manager call in a race
            // (0x1EE86) and goes to the simulator; the results screen's (0x1EBF0, 0x1EBFC) calls it at once: the substep
            // from here to the end of the vehicle manager call, as the approved release had it, the frame's time after
            if (clock_ret == 0x0001EBFC || clock_ret == 0x0001EE55) Mc3PhysEnterSub();
        }
    }
    if (fixed) {
        // a frame time past half a second is a load: left out of the summary
        if (measured < 0.5) {
            g_real += measured;
            g_game += Float(R32(kFrameTime));
            g_frames++;
        }
        g_since += measured;
        if (g_since >= 10.0) {
            if (g_frames)
                LOGI("timing: %u fixed step frames (%u advanced more than one step, %u waited for theirs), %.2f game s over %.2f real "
                 "s (x%.2f), step %.4f s",
                     g_frames, g_late, g_waits, g_game, g_real, g_real > 0 ? g_game / g_real : 0.0, g_original_step);
            g_real = g_game = g_since = 0;
            g_frames = g_late = g_waits = 0;
        }
    }
}
