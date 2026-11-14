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
const double kMaxFrameStep = 1.0 / 15.0 + 1e-4;  // the most a frame advances: the PC port's largest, 4 fields of 1/60
const size_t kMaxReplayFrames = 1u << 20;

float g_original_step = 0;  // what the game asked for, 1 / (rate * substeps)
double g_carry = 0;
std::vector<uint32_t> g_replay_steps;  // by recorded frame: the step bits it ran with
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
X86_EXTERN_FUNC(__imp__sub_0020B6F0);
X86_FUNC(sub_0020B6F0) {
    uint32_t now = uint32_t(x86_rdtsc());
    double measured = double(uint32_t(now - R32(kPrevTick))) / kTicksPerSecond;
    const bool fixed = R32(kFixedStep) != 0 && g_original_step > 0 && g_original_step < 0.2f;
    if (fixed) {
        const double step = g_original_step;
        double max_steps = floor(kMaxFrameStep / step);
        if (max_steps < 1) max_steps = 1;
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
            const double want = measured + g_carry;
            double k = floor(want / step + 0.5);
            if (k < 1) k = 1;
            if (k > max_steps) k = max_steps;
            n = uint32_t(k);
            g_carry = want - k * step;
            if (g_carry > step) g_carry = step;
            if (g_carry < -step) g_carry = -step;
        } else {
            g_carry = 0;  // a load or a pause: one step, as the console's
        }
        W32(kStep, Bits(float(n * step)));
        if (ReplayStep()) g_carry = 0;
        if (n > 1) g_late++;
    } else {
        g_carry = 0;
    }
    __imp__sub_0020B6F0(ctx, base);
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
