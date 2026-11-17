// DirectSound (XDK 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
// Xbox 360 build (see x360/tools/port_dsound.py)
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <math.h>
#include <vector>

#include "host.h"

#include "audio/audio.h"
#include "audio/modulation.h"
#include "audio/stream_status.h"
#include "audio/voice3d.h"
#include "xbe/xbe.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace kernel {
extern const xbe::Image* g_image;
}
namespace dispatch {
X86Func* Find(uint32_t guest);
}

// configs written before the SDK knew the name
#ifndef SYM_IDirectSound_SynchPlayback
#define SYM_IDirectSound_SynchPlayback FFFFFF00
#define HAS_IDirectSound_SynchPlayback 0
#endif
#ifndef SYM_IDirectSound_DownloadEffectsImage
#define SYM_IDirectSound_DownloadEffectsImage FFFFFF01
#define HAS_IDirectSound_DownloadEffectsImage 0
#endif
#ifndef SYM_IDirectSound_GetEffectData
#define SYM_IDirectSound_GetEffectData FFFFFF02
#define HAS_IDirectSound_GetEffectData 0
#endif
#ifndef SYM_IDirectSound_CommitEffectData
#define SYM_IDirectSound_CommitEffectData FFFFFF03
#define HAS_IDirectSound_CommitEffectData 0
#endif
#ifndef SYM_IDirectSound_GetOutputLevels
#define SYM_IDirectSound_GetOutputLevels FFFFFF04
#define HAS_IDirectSound_GetOutputLevels 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetPlayRegion
#define SYM_IDirectSoundBuffer_SetPlayRegion FFFFFF05
#define HAS_IDirectSoundBuffer_SetPlayRegion 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetAllParameters
#define SYM_IDirectSoundBuffer_SetAllParameters FFFFFF06
#define HAS_IDirectSoundBuffer_SetAllParameters 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetFormat
#define SYM_IDirectSoundBuffer_SetFormat FFFFFF07
#define HAS_IDirectSoundBuffer_SetFormat 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetOutputBuffer
#define SYM_IDirectSoundBuffer_SetOutputBuffer FFFFFF08
#define HAS_IDirectSoundBuffer_SetOutputBuffer 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetRolloffCurve
#define SYM_IDirectSoundBuffer_SetRolloffCurve FFFFFF09
#define HAS_IDirectSoundBuffer_SetRolloffCurve 0
#endif
#ifndef SYM_IDirectSoundBuffer_StopEx
#define SYM_IDirectSoundBuffer_StopEx FFFFFF0A
#define HAS_IDirectSoundBuffer_StopEx 0
#endif
#ifndef SYM_DirectSoundUseLightHRTF
#define SYM_DirectSoundUseLightHRTF FFFFFF0B
#define HAS_DirectSoundUseLightHRTF 0
#endif
#ifndef SYM_IDirectSoundStream_SetEG
#define SYM_IDirectSoundStream_SetEG FFFFFF0C
#define HAS_IDirectSoundStream_SetEG 0
#endif

// MC3 X-19: sounds the mixer played last pass, for the log's pulse (core/crash.cpp)
volatile uint32_t g_ds_playing_now = 0;

namespace {

template <typename T>
inline T ClampT(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }

const uint32_t kStreamVtable = SYM(DSound_StreamVtable);  // CDirectSoundStream XMediaObject vtable
const uint32_t kStreamVtable2 = SYM(DSound_StreamVtable2);
const uint32_t kPacketPending = 0x8000000A, kFlushed = 0x80004004;  // E_PENDING, E_ABORT

struct Packet {
    uint32_t data, size, completed_ptr, status_ptr, context;
    uint32_t used;
};

struct Voice {
    uint32_t obj;
    bool stream;
    int refs;
    // format
    uint16_t tag, channels, bits, align;
    uint32_t rate;
    // buffer
    // play region in bytes from data, loop region in bytes from the play region
    uint32_t data, bytes, loop_start, loop_len, play_start, play_len;
    uint32_t owned;  // memory the library made for the buffer
    bool playing, looping, paused;
    bool sync;  // held until IDirectSound_SynchPlayback
    bool active, ending;  // stream: took a packet since it went quiet, quiet again once they run out
    bool hold;            // stream: DSSTREAMPAUSE_PAUSENOACTIVATE, packets queue without the voice starting (PC port)
    bool stopping;        // stream: FlushEx released its envelope, it ends with the release or its last packet
    int64_t starved_at;   // stream: steady ms its packets ran out with no Discontinuity, -1 while fed
    double pos;  // frames
    uint32_t freq;
    int32_t pitch;        // SetPitch, 4096 an octave
    bool pitch_override;
    // stream
    uint32_t max_packets, callback, context;
    std::deque<Packet> packets;
    // levels
    float volume, headroom;
    uint8_t nbins, bin[8];
    int32_t bin_mb[32];  // by mix bin
    std::vector<float> curve;  // rolloff
    bool is3d;
    uint32_t mode;  // DS3DMODE: 1 head relative, 2 disabled
    float p[3], min_dist, max_dist;
    audio::Voice3DData voice3d;  // Set3DVoiceData / Use3DVoiceData (PC port)
    // SetEG / SetLFO, restarted at voice on: a buffer's Play while stopped, a stream's first packet since it went quiet
    audio::Modulation mod;
    // ADPCM block cache (2 slots to avoid thrashing at sample and packet boundaries)
    int64_t adpcm_block[2];
    int16_t adpcm_decoded[2][65 * 8];
    int adpcm_lru;
    Voice()
        : obj(0), stream(false), refs(1), tag(1), channels(1), bits(16), align(2), rate(48000), data(0), bytes(0),
          loop_start(0), loop_len(0), play_start(0), play_len(0), owned(0), playing(false), looping(false),
          paused(false), sync(false), active(false), ending(false), hold(false), stopping(false), starved_at(-1), pos(0),
          freq(0), pitch(0), pitch_override(false), max_packets(1), callback(0),
          context(0), volume(1), headroom(1), nbins(0), is3d(false), mode(0), min_dist(1), max_dist(1e9f), adpcm_lru(0) {
        memset(bin, 0, sizeof(bin));
        memset(bin_mb, 0, sizeof(bin_mb));
        memset(p, 0, sizeof(p));
        adpcm_block[0] = adpcm_block[1] = -1;
    }
    void ResetAdpcm() {
        adpcm_block[0] = -1;
        adpcm_block[1] = -1;
        adpcm_lru = 0;
    }
};

struct Done {
    uint32_t voice, callback, context, packet_context, status;
};

std::recursive_mutex g_lock;
// JSRF: o mixer segura g_lock enquanto mistura todas as vozes. Uma thread do jogo que esperava g_lock com a vez na mao
// parava as outras junto: no console, a thread do CRI ficava em GetCurrentPosition e a do jogo esperava a vez ate 20% do
// tempo nos picos da corrida (console_boot17). Ocupado, espera sem a vez e so pega g_lock de novo depois de ter a vez,
// para nunca segurar g_lock esperando a vez (o mixer pararia).
std::atomic<int64_t> g_ds_wait_ns(0), g_mix_hold_ns(0);
std::atomic<int> g_ds_waiters(0);
struct DsLock {
    DsLock() {
        if (g_lock.try_lock()) return;
        int64_t t = host::NowNs();
        g_ds_waiters++;
        for (;;) {
            {
                GuestUnlock unlocked;
                g_lock.lock();
                g_lock.unlock();
            }
            if (g_lock.try_lock()) break;
        }
        g_ds_waiters--;
        if (GuestHeld()) g_ds_wait_ns += host::NowNs() - t;
    }
    ~DsLock() { g_lock.unlock(); }
};
struct MixHold {
    int64_t t;
    MixHold() : t(host::NowNs()) {}
    ~MixHold() { g_mix_hold_ns += host::NowNs() - t; }
};
std::map<uint32_t, Voice> g_voices;
float g_listener[3] = {}, g_front[3] = {0, 0, 1}, g_top[3] = {0, 1, 0};
// Completed packets' callbacks, run as the XDK runs them: in DirectSoundDoWork on the title's thread, so they never
// overlap the title's own stream code. A stream flushed or released runs its own at once, before it is gone.
std::deque<Done> g_done;
struct DoneSignal { void notify_one() {} } g_done_cv;  // the pump polls every 25 ms
std::vector<Done>* FlushedList();  // per thread, run by the STREAM call that flushed them  // run by the STREAM call that flushed them, once g_lock is let go
std::atomic<int64_t> g_pumped(0);          // the last DirectSoundDoWork, steady ms
bool g_started = false;
uint32_t g_ds = 0;

float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : powf(10.0f, hundredths / 2000.0f); }

uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == 0x69) return bytes / (36u * v.channels) * 64;  // WAVE_FORMAT_XBOX_ADPCM
    return bytes / std::max<uint32_t>(v.align, 1);
}
uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == 0x69) return frames / 64 * 36 * v.channels;
    return frames * v.align;
}

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, the voice plays 64 of its 65 frames
void DecodeBlock(const uint8_t* b, int ch, int16_t* out) {
    for (int c = 0; c < ch; c++) {
        int pred = int16_t(b[c * 4] | b[c * 4 + 1] << 8), idx = std::min<int>(b[c * 4 + 2], 88);
        out[c] = int16_t(pred);
        const uint8_t* d = b + ch * 4;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kStep[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = ClampT(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = ClampT(idx + kIndex[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = int16_t(pred);
            }
    }
}

// sample of channel c at frame f of memory starting at data
// MC3 (2026-11-13): the mixer's inner loop calls this four times per output frame and voice (the mixer held g_lock
// 40 to 54% of the time on the console). Forced inline, and the scale by 1/32768 and 1/128 is a multiply: the runtime
// is built without /fp:fast, so the divisions were real ones, among the slowest instructions here. Same values (the
// reciprocals of powers of two are exact).
// and an int to float conversion on the Xenon goes through memory (a load-hit-store, tens of cycles): 16 and 8 bit
// samples come from tables of the same values instead
struct SampleTables {
    float s16[65536], u8[256];
    SampleTables() {
        for (int i = 0; i < 65536; i++) s16[i] = float(int16_t(uint16_t(i))) * (1.0f / 32768.0f);
        for (int i = 0; i < 256; i++) u8[i] = float(i - 128) * (1.0f / 128.0f);
    }
};
static SampleTables g_st;
__forceinline float Sample(Voice& v, uint32_t data, uint32_t f, int c) {
    c = std::min<int>(c, v.channels - 1);
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    if (v.tag == 0x69) {
        int64_t at = int64_t(data) + int64_t(f / 64) * 36 * v.channels;
        int slot = -1;
        if (at == v.adpcm_block[0]) {
            slot = 0;
        } else if (at == v.adpcm_block[1]) {
            slot = 1;
        } else {
            slot = (v.adpcm_lru == 0) ? 1 : 0;
            DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), v.adpcm_decoded[slot]);
            v.adpcm_block[slot] = at;
        }
        v.adpcm_lru = slot;
        return g_st.s16[uint16_t(v.adpcm_decoded[slot][(f % 64) * v.channels + c])];
    }
    if (v.bits == 8) return g_st.u8[p[f * v.align + c]];
    const uint8_t* q = p + f * v.align + c * 2;
    return g_st.s16[uint16_t(q[0] | q[1] << 8)];
}

void Complete(Voice& v, Packet& pk, uint32_t status) {
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        Done d = {v.obj, v.callback, v.context, pk.context, status};
        g_done.push_back(d);
        g_done_cv.notify_one();
    } else if (pk.context) {
        ObjectRef e = FromHandle(pk.context);
        if (e) SetEvent(*e);
    }
}

// gain of voice channel c on host left and right. A hardware voice holds two channels and owns a run of slots,
// which alternate left and right.
void Levels(Voice& v, float g[2][2]) {
    static const uint8_t kFront[2] = {0, 1};
    const uint8_t* bin = v.nbins ? v.bin : kFront;
    uint32_t n = v.nbins ? v.nbins : 2, voices = (std::max<uint32_t>(v.channels, 1) - 1) / 2 + 1;
    uint32_t per = std::max<uint32_t>(n / voices, 2), step = v.channels == 1 ? 1 : 2;
    bool speakers = false;
    for (uint32_t s = 0; s < n; s++) speakers |= bin[s] < 10 && bin[s] != 3;
    for (uint32_t c = 0; c < 2; c++) {
        g[c][0] = g[c][1] = 0;
        // MC3 (2026-11-16): the fold's weights squared that reach each side. A stereo buffer of the race on bins 0 to 5
        // had its left channel on front, centre and back (1 + 0.707 + 0.707 added: 1.33 on the left) and its right on
        // front and back (0.94), the sub left out: louder on the left and past 1.0 alone (Xenia log of 2026-11-16, bins
        // diagnostic), the mix clipped. The fold keeps the power instead: a side that several bins reach is divided by
        // the root of their weights squared (1.41 for front + centre + back, 1.22 for front + back); one bin is as before.
        float w2[2] = {0, 0};
        for (uint32_t s = c; c < v.channels && s < per && s < n; s += step) {
            uint32_t b = bin[s];
            if (speakers && (b == 3 || b > 9)) continue;  // sub and effect sends
            // the console's stereo downmix: centre, back pair and back crosstalk at -3 dB
            bool folded = b == 2 || b == 4 || b == 5 || b == 8 || b == 9;
            float x = Db(v.bin_mb[b]) * v.volume * v.headroom * (folded ? 0.707f : 1.0f) * v.voice3d.Gain(b);
            // even bins are left, odd right, 2 is the centre
            bool left = !speakers || b == 2 || !(b & 1);
            bool right = !speakers || b == 2 || (b & 1);
            // slots that reach one speaker add, as on the DSP; effect sends alone play the loudest
            if (speakers) {
                const float w = folded ? 0.5f : 1.0f;
                if (left) {
                    g[c][0] += x;
                    w2[0] += w;
                }
                if (right) {
                    g[c][1] += x;
                    w2[1] += w;
                }
            } else {
                g[c][0] = std::max(g[c][0], x);
                g[c][1] = std::max(g[c][1], x);
            }
        }
        for (int side = 0; side < 2; side++)
            if (w2[side] > 1.0f) g[c][side] /= sqrtf(w2[side]);
    }
    if (v.voice3d.enabled) return;
    if (!v.is3d || v.mode == 2) return;
    bool rel = v.mode == 1;  // the position is already in listener space
    float d[3] = {v.p[0], v.p[1], v.p[2]};
    for (int i = 0; i < 3 && !rel; i++) d[i] -= g_listener[i];
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float att = 1;
    if (dist > v.min_dist && v.curve.empty()) {
        att = v.min_dist / std::min(dist, v.max_dist);
    } else if (dist > v.min_dist && v.max_dist > v.min_dist) {
        // points spread evenly from min to max distance, 1.0 at min
        float at = (std::min(dist, v.max_dist) - v.min_dist) / (v.max_dist - v.min_dist) * v.curve.size();
        size_t i = std::min(size_t(at), v.curve.size() - 1);
        float from = i ? v.curve[i - 1] : 1.0f;
        att = from + (v.curve[i] - from) * (at - i);
    }
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    float side = rel ? d[0] : d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
    float pan = dist > 1e-3f ? side / dist : 0.0f;
    for (int c = 0; c < 2; c++) {
        g[c][0] *= att * std::min(1.0f, 1.0f - pan);
        g[c][1] *= att * std::min(1.0f, 1.0f + pan);
    }
}

// a stream goes quiet: the synch hold ends, a started one loses its pause as well
int64_t NowMs() { return host::NowNs() / 1000000; }

void Deactivate(Voice& v) {
    v.sync = false;
    v.starved_at = -1;
    v.stopping = false;
    if (v.active) v.active = v.paused = v.ending = false;
}

// the amplitude envelope's release ran out and the hardware turned the voice off. A buffer stops where it is; a
// stream's packets still queued complete as flushed, their callbacks run in DirectSoundDoWork as any others.
void ReleaseEnded(Voice& v) {
    if (!v.stream) {
        v.playing = false;
        return;
    }
    for (size_t k = 0; k < v.packets.size(); k++) Complete(v, v.packets[k], kFlushed);
    v.packets.clear();
    v.pos = 0;
    Deactivate(v);
}

// Two stereo streams (the format of every song) fed and audible together for longer than a crossfade is two songs
// at once. Logged once per episode so a run can be checked without listening.
void WatchOverlap() {
    static int64_t since = -1;
    static bool reported = false;
    static unsigned episodes = 0;
    uint32_t ids[2] = {0, 0}, n = 0;
    for (std::map<uint32_t, Voice>::iterator it = g_voices.begin(); it != g_voices.end(); ++it) {
        const Voice& v = it->second;
        if (v.stream && v.channels == 2 && v.active && !v.hold && !v.paused && !v.sync && !v.packets.empty() &&
            !v.stopping && v.volume > 0.01f && n < 2)
            ids[n++] = v.obj;
    }
    if (n < 2) {
        since = -1;
        reported = false;
        return;
    }
    const int64_t now = NowMs();
    if (since < 0) since = now;
    if (!reported && now - since > 3000) {
        reported = true;
        if (episodes++ < 16) LOGW("dsound: stereo streams %08X and %08X both playing for over 3 s", ids[0], ids[1]);
    }
}

double Log2(double x) { return log(x) * 1.4426950408889634; }
int32_t RoundI(double x) { return int32_t(floor(x + 0.5)); }

// 4-point, 3rd-order Hermite cubic interpolation for high-fidelity audio resampling
inline float InterpHermite(float p0, float p1, float p2, float p3, float t) {
    float c0 = p1;
    float c1 = 0.5f * (p2 - p0);
    float c2 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    float c3 = 0.5f * (p3 - p0) + 1.5f * (p1 - p2);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

Voice* Find(uint32_t obj);
float F32(uint32_t v);
// JSRF: ajustes de voz que o jogo faz a cada passo, para dezenas de vozes. Com o g_lock, a thread do jogo esperava a mixagem
// inteira: no console, 9% dela na corrida de Dogenzaka (tests/evidence/console_boot11). Agora vao para uma fila curta, com trava
// propria, e valem na proxima mixagem, antes de qualquer som dela.
// MC3 (2026-11-12): SetPitch and SetMixBinVolumes too. Called for every car sound each step, they took g_lock and waited
// for the voice being mixed: on the console the mixer held g_lock 26 to 39% of the race and the game threads waited
// 60 to 200 ms every 5 s (mc3_360.log of 2026-11-12 05:54). A bins entry carries up to 2 (bin, volume) pairs.
enum { kPendVolume, kPendFreq, kPendMin, kPendMax, kPendPos, kPendListener, kPendOrient, kPendPitch, kPendBins };
struct Pend {
    uint32_t obj, kind, v[6];
};
const uint32_t kPendMax_ = 2048;
Pend g_pend[kPendMax_];
uint32_t g_pend_n = 0;
CRITICAL_SECTION g_pend_lock;
struct PendInit {
    PendInit() { InitializeCriticalSection(&g_pend_lock); }
} g_pend_init;

void DrainPendLocked() {
    static Pend copy[kPendMax_];
    uint32_t n;
    EnterCriticalSection(&g_pend_lock);
    n = g_pend_n;
    if (n) memcpy(copy, g_pend, n * sizeof(Pend));
    g_pend_n = 0;
    LeaveCriticalSection(&g_pend_lock);
    for (uint32_t i = 0; i < n; i++) {
        const Pend& p = copy[i];
        if (p.kind == kPendListener) {
            for (int k = 0; k < 3; k++) g_listener[k] = F32(p.v[k]);
            continue;
        }
        if (p.kind == kPendOrient) {
            for (int k = 0; k < 3; k++) {
                g_front[k] = F32(p.v[k]);
                g_top[k] = F32(p.v[3 + k]);
            }
            continue;
        }
        Voice* v = Find(p.obj);
        if (!v) continue;
        switch (p.kind) {
            case kPendVolume: v->volume = Db(int32_t(p.v[0])); break;
            case kPendFreq: v->freq = p.v[0]; v->pitch_override = false; break;
            case kPendMin: v->min_dist = F32(p.v[0]); break;
            case kPendMax: v->max_dist = F32(p.v[0]); break;
            case kPendPos:
                for (int k = 0; k < 3; k++) v->p[k] = F32(p.v[k]);
                break;
            case kPendPitch: v->pitch = int32_t(p.v[0]); v->pitch_override = true; break;
            case kPendBins:
                for (uint32_t k = 0; k < p.v[0]; k++) v->bin_mb[p.v[1 + k * 2] & 31] = int32_t(p.v[2 + k * 2]);
                break;
        }
    }
}

void QueuePend(uint32_t obj, uint32_t kind, const uint32_t* v, int n) {
    for (;;) {
        EnterCriticalSection(&g_pend_lock);
        if (g_pend_n < kPendMax_) {
            Pend& p = g_pend[g_pend_n++];
            p.obj = obj;
            p.kind = kind;
            for (int i = 0; i < n; i++) p.v[i] = v[i];
            LeaveCriticalSection(&g_pend_lock);
            return;
        }
        LeaveCriticalSection(&g_pend_lock);
        DsLock lock;
        DrainPendLocked();
    }
}

// JSRF: posicao de cada voz publicada pelo mixer ao fim de cada voz, e por toda chamada de buffer. GetCurrentPosition com o
// mixer no meio da mistura le daqui em vez de esperar: no console, a thread do jogo parava 5 a 9% do tempo nesse retorno
// (0x16D120) nos picos da corrida (console_boot17). Fica no maximo uma mistura atras, como a posicao do hardware.
struct PosVal {
    uint32_t play, write;
    uint32_t status;  // MC3: DSBSTATUS as GetStatus gives it
};
std::map<uint32_t, PosVal> g_pos;
CRITICAL_SECTION g_pos_cs;
struct PosInit {
    PosInit() { InitializeCriticalSection(&g_pos_cs); }
} g_pos_init;
void PosOf(const Voice& v, uint32_t& play, uint32_t& write) {
    play = BytesOf(v, uint32_t(v.pos));
    write = v.play_len ? (play + BytesOf(v, 64 * 4)) % v.play_len : 0;
}
// MC3 (2026-11-13): a buffer made or let go at an address drops what was published for it: XACT recycles its buffers,
// and a stale "playing" from the last one at that address held the music back (console video of 2026-11-13 20:27)
void ForgetPos(uint32_t obj) {
    EnterCriticalSection(&g_pos_cs);
    g_pos.erase(obj);
    LeaveCriticalSection(&g_pos_cs);
}
void PublishPos(const Voice& v) {
    PosVal pv;
    PosOf(v, pv.play, pv.write);
    pv.status = (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u);
    EnterCriticalSection(&g_pos_cs);
    g_pos[v.obj] = pv;
    LeaveCriticalSection(&g_pos_cs);
}

std::map<uint32_t, Voice>::iterator MixYield(std::map<uint32_t, Voice>::iterator it) {
    PublishPos(it->second);
    if (!g_ds_waiters.load()) return ++it;
    uint32_t key = it->first;
    g_lock.unlock();
    SwitchToThread();
    g_lock.lock();
    return g_voices.upper_bound(key);
}

// MC3 (2026-11-16): the mix's level. The output was clipped at 1.0 sample by sample (console log of 2026-11-16: peak
// 1.000 in every 5 s of the race, and still with 6 voices after the finish, where the sound turned harsh). A limiter
// takes the block's gain down at once when the mix would pass kCeil and brings it back over ~150 ms; what still passes
// (the block starts at the last gain) bends into the ceiling instead of being cut flat. Under kCeil nothing changes.
int g_gap_n = 0;
int64_t g_gap_ms = 0;
struct VoicePeak {
    float peak;
    uint32_t obj;
};
VoicePeak g_loud[3];
float g_mix_peak = 0, g_lim_gain = 1.0f, g_lim_min = 1.0f;
uint32_t g_clip_frames = 0;
void NoteLoud(const Voice& v, float peak) {
    if (peak <= g_loud[2].peak) return;
    for (int j = 0; j < 3; j++)  // one entry per voice, its loudest
        if (g_loud[j].obj == v.obj) {
            if (peak <= g_loud[j].peak) return;
            for (int m = j; m < 2; m++) g_loud[m] = g_loud[m + 1];
            g_loud[2].peak = 0;
            g_loud[2].obj = 0;
            break;
        }
    int k = 2;
    while (k > 0 && peak > g_loud[k - 1].peak) {
        g_loud[k] = g_loud[k - 1];
        k--;
    }
    g_loud[k].peak = peak;
    g_loud[k].obj = v.obj;
}
__forceinline float SoftCeil(float x) {
    const float knee = 0.85f, room = 1.0f - knee;
    const float m = x < 0 ? -x : x;
    if (m <= knee) return x;
    const float over = (m - knee) / room;
    const float y = knee + room * over / (1.0f + over);  // reaches 1.0 only at infinity
    return x < 0 ? -y : y;
}
void Limit(float* out, uint32_t frames) {
    const float kCeil = 0.95f;
    float peak = 0;
    for (uint32_t i = 0; i < frames * 2; i++) {
        const float m = out[i] < 0 ? -out[i] : out[i];
        if (m > peak) peak = m;
        if (m > 1.0f) g_clip_frames++;
    }
    if (peak > g_mix_peak) g_mix_peak = peak;
    // the gain this block's peak allows: down at once, back up toward it over ~150 ms (never above it or 1)
    const float target = peak > kCeil ? kCeil / peak : 1.0f;
    const float from = g_lim_gain;
    const float back = float(frames) / (audio::kRate * 0.15f);
    float to = target < from ? target : from + (target - from) * (back < 1.0f ? back : 1.0f);
    g_lim_gain = to;
    if (to < g_lim_min) g_lim_min = to;
    if (from >= 0.9999f && to >= 0.9999f && peak <= 0.85f) return;  // the usual case: nothing to do
    // down within the first 1.3 ms (no click, and the block's peak is already under), up across the whole block
    const uint32_t ramp = to < from ? (frames < 64 ? frames : 64) : frames;
    const float d = (to - from) / float(ramp);
    for (uint32_t i = 0; i < frames; i++) {
        const float gn = i < ramp ? from + d * float(i + 1) : to;
        out[i * 2] = SoftCeil(out[i * 2] * gn);
        out[i * 2 + 1] = SoftCeil(out[i * 2 + 1] * gn);
    }
}

void Render(float* out, uint32_t frames) {
    std::memset(out, 0, frames * 2 * sizeof(float));
    DsLock lock;
    MixHold hold;
    DrainPendLocked();
    WatchOverlap();
    // bring-up: voices that exist and voices that play, every 5 s
    static uint32_t diag_frames = 0, diag_max_playing = 0;
    uint32_t playing = 0;
    // JSRF: entre uma voz e outra, quem espera g_lock entra; a mixagem segue da voz seguinte a ultima feita
    for (std::map<uint32_t, Voice>::iterator it_ = g_voices.begin(); it_ != g_voices.end(); it_ = MixYield(it_)) {
        Voice& v = it_->second;
        if (v.paused || v.sync || v.hold || (!v.stream && !v.playing)) continue;
        if (v.stream && v.packets.empty() && !v.active) continue;  // never started or gone quiet: no voice runs
        uint32_t base = v.data + v.play_start;
        // MC3 X-19: a buffer whose memory is not there is skipped and logged, not read (a fault in the mixer)
        if (!v.stream && v.play_len && (!mem::IsCommitted(base) || !mem::IsCommitted(base + v.play_len - 1))) {
            LOG_FIRST(16, "dsound: obj=%08X plays %08X+%u, memory not committed: skipped", v.obj, base, v.play_len);
            v.playing = false;
            continue;
        }
        while (v.stream && !v.packets.empty()) {
            const Packet& pk = v.packets.front();
            if (!pk.size || (mem::IsCommitted(pk.data) && mem::IsCommitted(pk.data + pk.size - 1))) break;
            LOG_FIRST(16, "dsound: stream obj=%08X packet %08X+%u, memory not committed: dropped", v.obj, pk.data, pk.size);
            Packet bad = pk;
            v.packets.pop_front();
            Complete(v, bad, 0);
        }
        playing++;
        float g[2][2];
        Levels(v, g);
        double hz = v.freq ? v.freq : v.rate;
        double pitch = v.pitch_override ? v.pitch : 4096.0 * Log2(std::max(hz, 1.0) / 48000.0);
        // envelopes and LFOs move on the hardware's 32 sample frames of output time, whatever the source rate;
        // a voice with none programmed keeps its rate and gain without stepping them (the console's cost here)
        const bool idle = v.mod.Idle();
        double step = 48000.0 / audio::kRate * v.voice3d.PitchRatio(pitch + (idle ? 0.0 : v.mod.pitch));
        float gain = idle ? 1.0f : v.mod.gain;
        bool off = false;  // the amplitude envelope ended the voice
        float vpk = 0;     // this voice's loudest output sample in the block
        uint32_t i = 0;
        for (; i < frames; i++) {
            if (!idle) {
                if (v.mod.sample == 0) {
                    off = !v.mod.Step();
                    step = 48000.0 / audio::kRate * v.voice3d.PitchRatio(pitch + v.mod.pitch);
                    gain = v.mod.gain;
                }
                v.mod.sample = (v.mod.sample + 1) % audio::kFrameSamples;
                if (off) break;
            }
            uint32_t f = uint32_t(v.pos);
            float a, b, a1, b1;  // this frame and the next, interpolated between
            if (v.stream) {
                while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
                    Packet pk = v.packets.front();
                    v.packets.pop_front();
                    pk.used = pk.size;
                    Complete(v, pk, 0);
                    v.pos -= FramesOf(v, pk.size);
                    f = uint32_t(v.pos);
                }
                if (v.packets.empty()) {
                    v.pos = 0;
                    v.ResetAdpcm();
                    // a stopping stream out of data is done: its release cannot run on, and it must not wait for a
                    // refill as a stalled song does
                    if (v.ending || v.stopping) Deactivate(v);
                    else if (v.starved_at < 0) {
                        v.starved_at = NowMs();
                        static unsigned reported = 0;
                        if (reported++ < 16) LOGW("dsound: stream %08X ran out of packets mid-wave, still PLAYING for its refill", v.obj);
                    }
                    break;
                }
                Packet& pk = v.packets.front();
                a = Sample(v, pk.data, f, 0);
                b = Sample(v, pk.data, f, 1);
                if (f + 1 < FramesOf(v, pk.size)) {
                    a1 = Sample(v, pk.data, f + 1, 0);
                    b1 = Sample(v, pk.data, f + 1, 1);
                } else if (v.packets.size() > 1) {
                    a1 = Sample(v, v.packets[1].data, 0, 0);
                    b1 = Sample(v, v.packets[1].data, 0, 1);
                } else {
                    a1 = a;
                    b1 = b;
                }
            } else {
                uint32_t start = v.looping ? FramesOf(v, v.loop_start) : 0;
                uint32_t end = FramesOf(v, v.looping ? v.loop_start + v.loop_len : v.play_len);
                if (f >= end) {
                    if (!v.looping || end <= start) {
                        v.playing = false;
                        v.pos = 0;
                        break;
                    }
                    v.pos = start + fmod(v.pos - start, double(end - start));
                    f = uint32_t(v.pos);
                }
                if (!v.data) break;
                a = Sample(v, base, f, 0);
                b = Sample(v, base, f, 1);
                uint32_t next = f + 1 < end ? f + 1 : v.looping ? start : f;
                a1 = Sample(v, base, next, 0);
                b1 = Sample(v, base, next, 1);
            }
            float t = float(v.pos - f);
            a = (a + (a1 - a) * t) * gain;
            b = (b + (b1 - b) * t) * gain;
            const float l = a * g[0][0] + b * g[1][0], r = a * g[0][1] + b * g[1][1];
            out[i * 2] += l;
            out[i * 2 + 1] += r;
            const float ml = l < 0 ? -l : l, mr = r < 0 ? -r : r;
            vpk = ml > vpk ? ml : vpk;
            vpk = mr > vpk ? mr : vpk;
            v.pos += step;
        }
        // a starved stream's voice still runs, and so do its envelopes, for the rest of the block
        bool running = v.stream ? v.active : v.playing;
        for (uint32_t k = i + 1; !idle && running && !off && k < frames; k++) {
            if (v.mod.sample == 0) off = !v.mod.Step();
            v.mod.sample = (v.mod.sample + 1) % audio::kFrameSamples;
        }
        NoteLoud(v, vpk);
        if (off) ReleaseEnded(v);
    }
    diag_max_playing = std::max(diag_max_playing, playing);
    g_ds_playing_now = playing;
    if ((diag_frames += frames) >= audio::kRate * 5) {
        LOGI("dsound: %u voices, up to %u playing in the last 5 s; mixer com g_lock %.0f ms, threads do jogo esperando g_lock %.0f ms",
             unsigned(g_voices.size()), diag_max_playing, double(g_mix_hold_ns.exchange(0)) / 1e6, double(g_ds_wait_ns.exchange(0)) / 1e6);
        LOGI("dsound: mix peak %.2f, %u samples over 1.0, limiter down to %.2f; streams refilled late %d times (%lld ms silent)",
             double(g_mix_peak), g_clip_frames, double(g_lim_min), g_gap_n, (long long)g_gap_ms);
        for (int k = 0; k < 3 && g_loud[k].peak > 0; k++) {
            const Voice* lv = Find(g_loud[k].obj);
            if (!lv) continue;
            LOGI("dsound:   loud %d: obj=%08X peak %.2f %s tag %X %u ch %u Hz vol %.2f headroom %.2f bins %u 3d %d", k,
                 g_loud[k].obj, double(g_loud[k].peak), lv->stream ? "stream" : "buffer", lv->tag, lv->channels, lv->rate,
                 double(lv->volume), double(lv->headroom), unsigned(lv->nbins), int(lv->is3d || lv->voice3d.enabled));
            char bins[160];
            int o = 0;
            for (int q = 0; q < lv->nbins && o < 140; q++) o += sprintf(bins + o, " %u:%d", lv->bin[q], lv->bin_mb[lv->bin[q]]);
            bins[o] = 0;
            float lg[2][2];
            Levels(*const_cast<Voice*>(lv), lg);
            LOGI("dsound:     bins (bin:mB)%s; gains L->(%.2f %.2f) R->(%.2f %.2f)", bins, double(lg[0][0]), double(lg[0][1]),
                 double(lg[1][0]), double(lg[1][1]));
        }
        memset(g_loud, 0, sizeof(g_loud));
        g_mix_peak = 0;
        g_lim_min = 1.0f;
        g_clip_frames = 0;
        g_gap_n = 0;
        g_gap_ms = 0;
        diag_frames = 0;
        diag_max_playing = 0;
    }
    Limit(out, frames);
    for (uint32_t i = 0; i < frames * 2; i++) out[i] = out[i] < -1.0f ? -1.0f : out[i] > 1.0f ? 1.0f : out[i];
}

DWORD g_flushed_tls = TlsAlloc();
std::vector<Done>* FlushedList() {
    std::vector<Done>* l = static_cast<std::vector<Done>*>(TlsGetValue(g_flushed_tls));
    if (!l) {
        l = new std::vector<Done>;
        TlsSetValue(g_flushed_tls, l);
    }
    return l;
}

void Run(X86Context& ctx, const std::vector<Done>& done) {
    for (size_t i = 0; i < done.size(); i++) CallGuest(ctx, done[i].callback, done[i].context, done[i].packet_context, done[i].status);
}

void Start() {
    if (g_started) return;
    g_started = true;
    audio::StartOutput(Render);
    // for a title that never calls DirectSoundDoWork: its callbacks come from here instead
    StartHostThread("dsound", [](X86Context& ctx) {
        const int64_t kPumpMs = 25;
        for (;;) {
            std::vector<Done> done;
            {
                host::SleepNs(25000000);
                DsLock lock;
                if (NowMs() - g_pumped < kPumpMs) continue;
                done.assign(g_done.begin(), g_done.end());
                g_done.clear();
            }
            Run(ctx, done);
        }
    });
}

// WAVEFORMATEX
void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + 2), 1);
    v.rate = R32(wfx + 4);
    v.align = std::max<uint16_t>(R16(wfx + 12), 1);
    v.bits = R16(wfx + 14);
    if (v.tag != 1 && v.tag != 0x69) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// DSMIXBINS: the voice's bins and their volumes
void SetBins(Voice& v, uint32_t mixbins) {
    v.nbins = 0;  // none listed: front left and right
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 8), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v.bin[i] = R32(pairs + i * 8) & 31;
        v.bin_mb[v.bin[i]] = int32_t(R32(pairs + i * 8 + 4));
    }
    v.nbins = uint8_t(n);
}
// volumes only, the bins stay
void SetBinVolumes(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v.bin_mb[R32(pairs + i * 8) & 31] = int32_t(R32(pairs + i * 8 + 4));
}
// 600 mB unless submix or effect in buffer
float DefaultHeadroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : Db(-600); }  // 3D too (PC port)

Voice* Find(uint32_t obj) {
    auto it = g_voices.find(obj);
    return it == g_voices.end() ? nullptr : &it->second;
}

uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    DsLock lock;
    Start();
    uint32_t obj = mem::AllocSystem(0x40);
    std::memset(HostPtr(obj), 0, 0x40);
    ForgetPos(obj);
    Voice& v = g_voices[obj];
    v.obj = obj;
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8);  // DSBUFFERDESC: size, flags, bytes, format, mix bins
    ReadFormat(v, R32(desc + 12));
    SetBins(v, R32(desc + 16));
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    if (bytes) {
        v.data = v.owned = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 4);
        v.bytes = v.play_len = v.loop_len = bytes;
    }
    W32(out, obj);
    LOG_FIRST(16, "dsound: NewBuffer obj=%08X bytes=%u tag=%u rate=%u ch=%u flags=%08X",
              obj, bytes, v.tag, v.rate, v.channels, flags);
    return 0;
}

float F32(uint32_t v) {
    union { uint32_t u; float f; } c;
    c.u = v;
    return c.f;
}

uint32_t GetVoiceProperties(const Voice& v, uint32_t out) {
    if (!out) return 0x80070057u;
    // DSVOICEPROPS is 0x5C bytes, including eight inline DSMIXBINVOLUMEPAIR entries.
    std::memset(HostPtr(out), 0, 0x5C);
    uint32_t n = v.nbins ? v.nbins : 2;
    W32(out, n);
    float level = v.volume * v.headroom;
    int32_t mb = level > 0 ? RoundI(2000.0 * log10(double(level))) : -10000;
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t bin = i < n ? (v.nbins ? v.bin[i] : i) : 0xFFFFFFFFu;
        W32(out + 4 + i * 8, bin);
        int32_t volume = i < n ? int32_t(audio::ClampI64(int64_t(mb) + v.bin_mb[bin & 31] + v.voice3d.Attenuation(bin), -10000, 0)) : -10000;
        W32(out + 8 + i * 8, uint32_t(volume));
    }
    double hz = v.freq ? v.freq : v.rate;
    int32_t pitch = v.pitch_override ? v.pitch : RoundI(4096.0 * Log2(std::max(hz, 1.0) / 48000.0));
    int64_t combined_pitch = int64_t(pitch) + (v.voice3d.enabled ? v.voice3d.DopplerPitch() : 0);
    W32(out + 0x44, uint32_t(int32_t(audio::ClampI64(combined_pitch, -32767, 8191))));
    if (v.voice3d.enabled) {
        W32(out + 0x48, v.voice3d.words[1]);
        W32(out + 0x4C, v.voice3d.words[2]);
        W32(out + 0x50, v.voice3d.words[9]);
        W32(out + 0x54, v.voice3d.words[7]);
        W32(out + 0x58, v.voice3d.words[8]);
    }
    return 0;
}

uint32_t Set3DVoiceData(Voice& v, uint32_t data) {
    if (!data) return 0x80070057u;
    uint32_t words[14];
    for (int i = 0; i < 14; i++) words[i] = R32(data + i * 4);  // guest little endian
    v.voice3d.Set(words);
    return 0;
}

// DSENVELOPEDESC, ten dwords: generator, mode, delay, attack, hold, decay, release, sustain, pitch scale, cutoff scale.
// As CMcpxVoiceClient::SetEG (0x3C6F1B) packs it: generator 0 is the multi envelope, generator 1 the amplitude one.
// The times and the sustain take effect at once; the mode is only read at the next voice on.
uint32_t SetEG(Voice& v, uint32_t desc) {
    if (!desc) return 0x80070057u;
    uint32_t gen = R32(desc);
    if (gen > 1) return 0;
    audio::Envelope& e = gen ? v.mod.amp : v.mod.multi;
    e.mode = uint8_t(R32(desc + 4) & 15);
    e.delay = uint16_t(R32(desc + 8) & 0xFFF);
    e.attack = uint16_t(R32(desc + 12) & 0xFFF);
    e.hold = uint16_t(R32(desc + 16) & 0xFFF);
    e.decay = uint16_t(R32(desc + 20) & 0xFFF);
    e.release = uint16_t(R32(desc + 24) & 0xFFF);
    e.sustain = uint8_t(R32(desc + 28));
    if (!gen) {
        v.mod.pitch_scale = int8_t(uint8_t(R32(desc + 32)));
        v.mod.cutoff_scale = int8_t(uint8_t(R32(desc + 36)));
    }
    LOG_FIRST(16, "dsound: %08X %s envelope mode %u delay %u attack %u hold %u decay %u release %u sustain %u",
              v.obj, gen ? "amplitude" : "multi", unsigned(e.mode), unsigned(e.delay), unsigned(e.attack),
              unsigned(e.hold), unsigned(e.decay), unsigned(e.release), unsigned(e.sustain));
    return 0;
}

// DSLFODESC, six dwords: LFO, delay, delta, pitch, cutoff and amplitude modulation (CMcpxVoiceClient::SetLFO 0x3C6DEC)
uint32_t SetLFO(Voice& v, uint32_t desc) {
    if (!desc) return 0x80070057u;
    uint32_t which = R32(desc), delay = R32(desc + 4);
    if (which > 1) return 0;
    audio::Lfo& l = which ? v.mod.pitch_lfo : v.mod.multi_lfo;
    l.enabled = delay != 0;
    l.delay = uint16_t(delay & 0x7FFF);
    l.delta = uint16_t(R32(desc + 8) & 0x3FF);
    l.pitch = int8_t(uint8_t(R32(desc + 12)));
    if (!which) {
        l.cutoff = int8_t(uint8_t(R32(desc + 16)));
        l.amplitude = int8_t(uint8_t(R32(desc + 20)));
    }
    LOG_FIRST(8, "dsound: %08X %s LFO delay %u delta %u depths %d %d %d", v.obj, which ? "pitch" : "multi",
              unsigned(l.delay), unsigned(l.delta), int(l.pitch), int(l.cutoff), int(l.amplitude));
    return 0;
}

}  // namespace

// device

// DSOUND objects are the host's; .XTLID names linked exports that would still run console code on them
static void AuditExports() {
    static const uint32_t handled[] = {
        SYM(DirectSoundCreate), SYM(IDirectSound_Release), SYM(DirectSoundUseFullHRTF), SYM(DirectSoundUseLightHRTF),
        SYM(IDirectSound_GetSpeakerConfig),
        SYM(IDirectSound_SetEffectData), SYM(IDirectSound_EnableHeadphones), SYM(IDirectSound_SetMixBinHeadroom),
        SYM(IDirectSound_CommitDeferredSettings), SYM(IDirectSound_SetDopplerFactor), SYM(IDirectSound_SetRolloffFactor),
        SYM(IDirectSound_SetDistanceFactor), SYM(IDirectSound_SetVelocity), SYM(IDirectSound_SetI3DL2Listener),
        SYM(IDirectSound_SetPosition), SYM(IDirectSound_SetOrientation), SYM(IDirectSound_SynchPlayback), SYM(DirectSoundDoWork),
        SYM(IDirectSound_DownloadEffectsImage), SYM(IDirectSound_GetEffectData), SYM(IDirectSound_CommitEffectData),
        SYM(IDirectSound_GetOutputLevels), SYM(IDirectSoundBuffer_SetPlayRegion), SYM(IDirectSoundBuffer_SetAllParameters),
        SYM(IDirectSoundBuffer_SetFormat), SYM(IDirectSoundBuffer_SetOutputBuffer), SYM(IDirectSoundBuffer_SetRolloffCurve),
        SYM(IDirectSoundBuffer_StopEx),
        SYM(XAudioDownloadEffectsImage), SYM(DirectSoundCreateBuffer), SYM(IDirectSound_CreateSoundBuffer),
        SYM(IDirectSoundBuffer_Release), SYM(IDirectSoundBuffer_Unlock), SYM(IDirectSoundBuffer_SetBufferData),
        SYM(IDirectSoundBuffer_Play), SYM(IDirectSoundBuffer_Stop), SYM(IDirectSoundBuffer_SetLoopRegion),
        SYM(IDirectSoundBuffer_GetStatus), SYM(IDirectSoundBuffer_GetCurrentPosition), SYM(IDirectSoundBuffer_SetCurrentPosition),
        SYM(IDirectSoundBuffer_Lock), SYM(IDirectSoundBuffer_SetVolume), SYM(IDirectSoundBuffer_SetFrequency),
        SYM(IDirectSoundBuffer_SetFilter), SYM(IDirectSoundBuffer_SetHeadroom), SYM(IDirectSoundBuffer_SetMixBinVolumes),
        SYM(IDirectSoundBuffer_SetMixBins), SYM(IDirectSoundBuffer_SetMaxDistance), SYM(IDirectSoundBuffer_SetMinDistance),
        SYM(IDirectSoundBuffer_SetPosition), SYM(IDirectSoundBuffer_SetVelocity), SYM(IDirectSoundBuffer_SetI3DL2Source),
        SYM(DirectSoundCreateStream), SYM(CDirectSoundStream_AddRef), SYM(CDirectSoundStream_SetFormat),
        SYM(CDirectSoundStream_Release), SYM(CDirectSoundStream_GetInfo), SYM(CDirectSoundStream_GetStatus),
        SYM(CDirectSoundStream_Process), SYM(CDirectSoundStream_Discontinuity), SYM(CDirectSoundStream_Flush),
        SYM(CDirectSoundStream_FlushEx), SYM(CDirectSoundStream_Pause), SYM(CDirectSoundStream_SetVolume),
        SYM(CDirectSoundStream_SetFrequency), SYM(CDirectSoundStream_SetFilter), SYM(CDirectSoundStream_SetHeadroom),
        SYM(CDirectSoundStream_SetMixBinVolumes), SYM(CDirectSoundStream_SetMixBins), SYM(CDirectSoundStream_SetMaxDistance),
        SYM(CDirectSoundStream_SetMinDistance), SYM(CDirectSoundStream_SetPosition), SYM(CDirectSoundStream_SetVelocity),
        SYM(CDirectSoundStream_SetI3DL2Source), SYM(IDirectSoundStream_SetEG),
        // MC3, from the PC port: these ran the XDK's own code on the host's objects
        SYM(IDirectSound_CreateSoundStream), SYM(IDirectSound_GetCaps),
        SYM(IDirectSoundBuffer_GetVoiceProperties), SYM(IDirectSoundBuffer_Set3DVoiceData), SYM(IDirectSoundBuffer_Use3DVoiceData),
        SYM(CDirectSoundStream_GetVoiceProperties), SYM(CDirectSoundStream_Set3DVoiceData), SYM(CDirectSoundStream_Use3DVoiceData),
        SYM(IDirectSoundBuffer_SetPitch), SYM(IDirectSoundBuffer_SetLFO), SYM(IDirectSoundBuffer_SetEG),
        SYM(CDirectSoundStream_SetPitch), SYM(CDirectSoundStream_SetLFO), SYM(CDirectSoundStream_SetOutputBuffer)};
    std::string left;
    // the library's own section: the WMA decoder shares the export ids and touches no DSOUND object
    uint32_t lo = 0, hi = 0;
    for each (auto& s in g_image->sections)
        if (SYM(DirectSoundCreate) - s.va < s.vsize) lo = s.va, hi = s.va + s.vsize;
    for each (auto& s in g_image->sections)
        for (uint32_t o = 0; s.name == ".XTLID" && o + 8 <= std::min(s.vsize, s.rsize); o += 8) {
            uint32_t id = g_image->U32(s.raw + o), va = g_image->U32(s.raw + o + 4);
            auto done = [&](uint32_t f) { return std::find(std::begin(handled), std::end(handled), f) != std::end(handled); };
            // a jmp to a host version is covered by it
            if (id >> 16 != 0xB || va < lo || va >= hi || !dispatch::Find(va) || done(va) ||  // 0xB: DSOUND's library id
                (G8(va) == 0xE9 && done(va + 5 + R32(va + 1))))
                continue;
            char b[24];
            _snprintf(b, sizeof b, " %05X at %08X", id, va);
            left += b;
        }
    if (!left.empty()) LOGW("dsound: exports running their original code:%s", left.c_str());
}

HLE_STDCALL(DirectSoundCreate, 3) {
    DsLock lock;
    Start();
    if (!g_ds) {
        AuditExports();
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
    }
    W32(a[1], g_ds);
    return 0;
}
// JSRF chama AddRef (0x19E4C6); o BRE nao chamava. O objeto e unico e do host, como em Release.
HLE_STDCALL(IDirectSound_AddRef, 1) { return 1; }
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
HLE_STDCALL(DirectSoundUseFullHRTF, 0) { return 0; }
HLE_STDCALL(DirectSoundUseLightHRTF, 0) { return 0; }
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    W32(a[1], 0);  // DSSPEAKER_STEREO
    return 0;
}
HLE_STDCALL(IDirectSound_SetEffectData, 6) { return 0; }
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) { return 0; }
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) { return 0; }
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) { return 0; }
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetDistanceFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetVelocity, 5) { return 0; }
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetPosition, 5) {
    uint32_t v[3] = {a[1], a[2], a[3]};
    QueuePend(0, kPendListener, v, 3);
    return 0;
}
HLE_STDCALL(IDirectSound_SetOrientation, 8) {
    uint32_t v[6] = {a[1], a[2], a[3], a[4], a[5], a[6]};
    QueuePend(0, kPendOrient, v, 6);
    return 0;
}
HLE_STDCALL(DirectSoundDoWork, 0) {
    g_pumped = NowMs();
    std::vector<Done> done;
    {
        DsLock lock;
        done.assign(g_done.begin(), g_done.end());
        g_done.clear();
    }
    Run(ctx, done);
    return 0;
}
// starts together every voice played or unpaused with the synch playback flag
HLE_STDCALL(IDirectSound_SynchPlayback, 1) {
    DsLock lock;
    for (std::map<uint32_t, Voice>::iterator it_ = g_voices.begin(); it_ != g_voices.end(); ++it_) it_->second.sync = false;
    return 0;
}

// Effects run on the console's DSP and are not reproduced.
// the descriptor keeps the image's effect list so the title finds its indices, every segment points at zeroed memory
static uint32_t EffectsDesc(const std::vector<uint8_t>& img, const char* name) {
    XDK_REQUIRE("dsound", XDK_KEY(EffectsImage_Skip0), XDK_KEY(EffectsImage_Skip1), XDK_KEY(EffectsImage_Desc));
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= img.size() ? uint32_t(img[o] | img[o + 1] << 8 | img[o + 2] << 16 | uint32_t(img[o + 3]) << 24) : 0; };
    size_t t = XDK_EffectsImage_Desc + size_t(u32(XDK_EffectsImage_Skip0) + u32(XDK_EffectsImage_Skip1)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    LOGI("dsound: effects image %s, %u effects", name, count);
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4);
            uint32_t seg = mem::AllocSystem(std::max(size, 0x40u));
            std::memset(HostPtr(seg), 0, std::max(size, 0x40u));
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    return desc;
}

HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        for each (auto& sec in g_image->sections)
            if (sec.name == name) img.assign(static_cast<uint8_t*>(HostPtr(sec.va)), static_cast<uint8_t*>(HostPtr(sec.va)) + sec.vsize);
    } else {
        ReadGameFile(name, img);
    }
    uint32_t desc = EffectsDesc(img, name.c_str());
    if (a[3]) W32(a[3], desc);
    return 0;
}

// (this, image, size, location, descriptor out): the title loaded the image itself
HLE_STDCALL(IDirectSound_DownloadEffectsImage, 5) {
    std::vector<uint8_t> img(static_cast<uint8_t*>(HostPtr(a[1])), static_cast<uint8_t*>(HostPtr(a[1])) + a[2]);
    uint32_t desc = EffectsDesc(img, "from memory");
    if (a[4]) W32(a[4], desc);
    return 0;
}
// (this, effect, offset, data, size): SetEffectData keeps nothing, so effect memory reads as zero
HLE_STDCALL(IDirectSound_GetEffectData, 5) {
    if (a[3]) std::memset(HostPtr(a[3]), 0, a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_CommitEffectData, 1) { return 0; }
// (this, DSOUTPUTLEVELS out, reset peaks): 16 dwords of meters, silent
HLE_STDCALL(IDirectSound_GetOutputLevels, 3) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 64);
    return 0;
}

// buffers

HLE_STDCALL(DirectSoundCreateBuffer, 2) { return NewBuffer(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundBuffer, 4) { return NewBuffer(a[1], a[2]); }
// DSCAPS: free 2D/3D voices and buffer pages, bytes allocated. MC3 asks; JSRF did not (port of MidnightClub3-Port).
HLE_STDCALL(IDirectSound_GetCaps, 2) {
    if (!a[1]) return 0x80070057u;
    DsLock lock;
    uint32_t two_d = 0, three_d = 0, pages = 0;
    for (std::map<uint32_t, Voice>::iterator it = g_voices.begin(); it != g_voices.end(); ++it) {
        if (it->second.is3d) ++three_d; else ++two_d;
        pages += (it->second.bytes + 4095) / 4096;
    }
    W32(a[1], 192 - std::min(two_d, 192u));
    W32(a[1] + 4, 64 - std::min(three_d, 64u));
    W32(a[1] + 8, 2047 - std::min(pages, 2047u));
    W32(a[1] + 12, pages * 4096);
    return 0;
}

#define BUFFER(name, nargs)                                 \
    static uint32_t buf_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(IDirectSoundBuffer_##name, nargs) {         \
        DsLock lock;                                        \
        Voice* v = Find(a[0]);                              \
        if (!v) return 0x80004005u;                         \
        uint32_t r = buf_##name(*v, a);                     \
        if ((v = Find(a[0])) != nullptr) PublishPos(*v);    \
        return r;                                           \
    }                                                       \
    static uint32_t buf_##name(Voice& v, const Args& a)

BUFFER(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    if (v.owned) mem::FreeContiguous(v.owned);
    uint32_t obj = v.obj;
    EnterCriticalSection(&g_pos_cs);
    g_pos.erase(obj);
    LeaveCriticalSection(&g_pos_cs);
    ForgetPos(obj);
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
BUFFER(AddRef, 1) { return uint32_t(++v.refs); }  // JSRF: 0x19E4F7
BUFFER(Unlock, 5) { return 0; }
BUFFER(GetVoiceProperties, 2) { return GetVoiceProperties(v, a[1]); }
BUFFER(Set3DVoiceData, 2) { return Set3DVoiceData(v, a[1]); }
BUFFER(Use3DVoiceData, 2) {
    v.voice3d.enabled = a[1] != 0;
    return 0;
}
void QueueBinVolumes(uint32_t obj, uint32_t mixbins);
HLE_STDCALL(IDirectSoundBuffer_SetPitch, 2) {
    uint32_t v[1] = {a[1]};
    QueuePend(a[0], kPendPitch, v, 1);
    return 0;
}
BUFFER(SetLFO, 2) { return SetLFO(v, a[1]); }
BUFFER(SetEG, 2) { return SetEG(v, a[1]); }
BUFFER(SetBufferData, 3) {
    if (a[1] == v.data && a[2] == v.bytes) {
        v.pos = 0;
        v.play_start = v.loop_start = 0;
        v.play_len = v.loop_len = a[2];
        v.ResetAdpcm();
        return 0;
    }
    // the library frees its own memory, and makes it again for a size with no data
    if (v.owned && v.owned != a[1]) {
        mem::FreeContiguous(v.owned);
        v.owned = 0;
    }
    v.data = a[1];
    if (!a[1] && a[2]) v.data = v.owned = mem::AllocContiguous(a[2], 0, 0xFFFFFFFF, 4);
    v.bytes = v.play_len = v.loop_len = a[2];
    v.play_start = v.loop_start = 0;
    v.pos = 0;
    v.ResetAdpcm();
    return 0;
}
BUFFER(Play, 4) {
    LOG_FIRST(24, "dsound: Play obj=%08X data=%08X bytes=%u play %u+%u loop %u+%u flags=%X", v.obj, v.data, v.bytes, v.play_start,
              v.play_len, v.loop_start, v.loop_len, a[3]);
    // VOICE_ON only for a voice that is not running (0x3C7CB5): one still releasing goes on releasing
    if (!v.playing) v.mod.On();
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    v.sync = a[3] & 4;
    return 0;
}
BUFFER(Stop, 1) {
    v.playing = false;
    return 0;
}
// (this, time low, time high, flags), the time is not scheduled. As 0x3C87ED: DSBSTOPEX_ENVELOPE (1) releases both
// envelopes (VOICE_RELEASE) and the buffer plays on until the amplitude one has run out, DSBSTOPEX_RELEASEWAVEFORM
// (2) with it also leaves the loop so the buffer plays out to its end. A zero release time stops it at once.
BUFFER(StopEx, 4) {
    if (!v.playing) return 0;
    if (!(a[3] & 1)) {
        v.playing = false;
        return 0;
    }
    if (a[3] & 2) v.looping = false;
    v.mod.Release();
    if (v.mod.Instant()) v.playing = false;
    return 0;
}
// (this, start, length) in bytes, 0 length plays to the end. The loop becomes the whole region.
BUFFER(SetPlayRegion, 3) {
    // MC3 X-19 (2026-11-11): as the PC port's dsound.cpp. The BRE took this call for SetBufferData whenever the buffer
    // had no bytes or the start passed 0x10000 (its 0x000FE060 was SetBufferData); MC3's really is SetPlayRegion, an
    // offset into the wave bank, and that turned it into an address: the title screen played code at 0x437B8
    if (a[1] > v.bytes || a[2] > v.bytes - a[1]) {
        LOG_FIRST(16, "dsound: SetPlayRegion obj=%08X %u+%u outside its %u bytes, refused", v.obj, a[1], a[2], v.bytes);
        return 0x88780032u;  // DSERR_INVALIDCALL
    }
    v.play_start = a[1];
    v.play_len = v.loop_len = a[2] ? a[2] : v.bytes - a[1];
    v.loop_start = 0;
    v.pos = 0;
    v.ResetAdpcm();
    return 0;
}
BUFFER(SetLoopRegion, 3) {
    if (a[1] > v.play_len || a[2] > v.play_len - a[1]) return 0x88780032u;
    v.loop_start = a[1];
    v.loop_len = a[2] ? a[2] : v.play_len - a[1];
    return 0;
}
// MC3 (2026-11-13): as GetCurrentPosition, the published status while the mixer holds g_lock. Through DsLock the call
// waited for the voice being mixed: 55 us each on the console (0.77 ms a frame for 14 calls, log of 2026-11-13).
HLE_STDCALL(IDirectSoundBuffer_GetStatus, 2) {
    uint32_t status = 0;
    if (g_lock.try_lock()) {
        Voice* v = Find(a[0]);
        if (v) status = (v->playing ? 1u : 0u) | (v->playing && v->looping ? 4u : 0u);  // DSBSTATUS_PLAYING, _LOOPING
        g_lock.unlock();
        if (!v) return 0x80004005u;
    } else {
        EnterCriticalSection(&g_pos_cs);
        std::map<uint32_t, PosVal>::iterator it = g_pos.find(a[0]);
        bool got = it != g_pos.end();
        if (got) status = it->second.status;
        LeaveCriticalSection(&g_pos_cs);
        if (!got) {
            DsLock lock;
            Voice* v = Find(a[0]);
            if (!v) return 0x80004005u;
            status = (v->playing ? 1u : 0u) | (v->playing && v->looping ? 4u : 0u);
        }
    }
    if (a[1]) W32(a[1], status);
    return 0;
}
HLE_STDCALL(IDirectSoundBuffer_GetCurrentPosition, 3) {
    PosVal pv;
    if (g_lock.try_lock()) {
        Voice* v = Find(a[0]);
        if (v) PosOf(*v, pv.play, pv.write);
        g_lock.unlock();
        if (!v) return 0x80004005u;
    } else {
        EnterCriticalSection(&g_pos_cs);
        std::map<uint32_t, PosVal>::iterator it = g_pos.find(a[0]);
        bool got = it != g_pos.end();
        if (got) pv = it->second;
        LeaveCriticalSection(&g_pos_cs);
        if (!got) {
            DsLock lock;
            Voice* v = Find(a[0]);
            if (!v) return 0x80004005u;
            PosOf(*v, pv.play, pv.write);
        }
    }
    if (a[1]) W32(a[1], pv.play);
    if (a[2]) W32(a[2], pv.write);
    return 0;
}
BUFFER(SetCurrentPosition, 2) {
    v.pos = FramesOf(v, a[1]);
    return 0;
}
BUFFER(Lock, 8) {
    uint32_t off = a[1], n = a[2];
    if (a[7] & 1) n = v.bytes;  // DSBLOCK_ENTIREBUFFER
    if (!v.bytes) return 0x80004005u;  // E_FAIL
    off %= v.bytes;
    uint32_t first = std::min(n, v.bytes - off);
    W32(a[3], v.data + off);
    W32(a[4], first);
    if (a[5]) W32(a[5], n > first ? v.data : 0);
    if (a[6]) W32(a[6], n > first ? n - first : 0);
    return 0;
}
HLE_STDCALL(IDirectSoundBuffer_SetVolume, 2) {
    uint32_t v[1] = {a[1]};
    QueuePend(a[0], kPendVolume, v, 1);
    return 0;
}
HLE_STDCALL(IDirectSoundBuffer_SetFrequency, 2) {
    uint32_t v[1] = {a[1]};
    QueuePend(a[0], kPendFreq, v, 1);
    return 0;
}
BUFFER(SetFilter, 2) { return 0; }
BUFFER(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
// the pairs are read now: the title may change its struct once the call returns
void QueueBinVolumes(uint32_t obj, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i += 2) {
        uint32_t v[6] = {0, 0, 0, 0, 0, 0}, k = std::min<uint32_t>(n - i, 2);
        v[0] = k;
        for (uint32_t j = 0; j < k; j++) v[1 + j * 2] = R32(pairs + (i + j) * 8), v[2 + j * 2] = R32(pairs + (i + j) * 8 + 4);
        QueuePend(obj, kPendBins, v, 1 + 2 * k);
    }
}
HLE_STDCALL(IDirectSoundBuffer_SetMixBinVolumes, 2) {
    QueueBinVolumes(a[0], a[1]);
    return 0;
}
BUFFER(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
HLE_STDCALL(IDirectSoundBuffer_SetMaxDistance, 3) {
    uint32_t v[1] = {a[1]};
    QueuePend(a[0], kPendMax, v, 1);
    return 0;
}
HLE_STDCALL(IDirectSoundBuffer_SetMinDistance, 3) {
    uint32_t v[1] = {a[1]};
    QueuePend(a[0], kPendMin, v, 1);
    return 0;
}
HLE_STDCALL(IDirectSoundBuffer_SetPosition, 5) {
    uint32_t v[3] = {a[1], a[2], a[3]};
    QueuePend(a[0], kPendPos, v, 3);
    return 0;
}
BUFFER(SetVelocity, 5) { return 0; }
BUFFER(SetI3DL2Source, 3) { return 0; }
// (this, DS3DBUFFER, apply): position at +4, distances at +0x34 and +0x38, mode at +0x3C
BUFFER(SetAllParameters, 3) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(R32(a[1] + 4 + i * 4));
    v.min_dist = F32(R32(a[1] + 0x34));
    v.max_dist = F32(R32(a[1] + 0x38));
    v.mode = R32(a[1] + 0x3C);
    return 0;
}
// (this, points, count, apply)
BUFFER(SetRolloffCurve, 4) {
    v.curve.clear();
    for (uint32_t i = 0; a[1] && i < std::min<uint32_t>(a[2], 64); i++) v.curve.push_back(F32(R32(a[1] + i * 4)));
    return 0;
}
BUFFER(SetOutputBuffer, 2) { return 0; }  // the voice keeps its own mix bins
BUFFER(SetFormat, 2) {
    ReadFormat(v, a[1]);
    v.freq = 0;
    v.pitch_override = false;
    v.ResetAdpcm();
    return 0;
}

// streams, the object carries the XDK's own vtable

// shared by DirectSoundCreateStream and IDirectSound_CreateSoundStream (MC3 creates its XACT streams through the
// interface, JSRF did not: same as MidnightClub3-Port runtime/xdk/dsound.cpp NewStream)
static uint32_t NewStream(uint32_t desc, uint32_t out) {
    DsLock lock;
    Start();
    if (!desc || !out) return 0x80070057u;
    uint32_t obj = mem::AllocSystem(0x80);
    std::memset(HostPtr(obj), 0, 0x80);
    W32(obj + XDK_CDirectSoundStream_Vtable, kStreamVtable);
    W32(obj + XDK_CDirectSoundStream_Vtable2, kStreamVtable2);
    ForgetPos(obj);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.stream = true;
    uint32_t flags = R32(desc);
    v.max_packets = std::max<uint32_t>(R32(desc + 4), 1);
    ReadFormat(v, R32(desc + 8));
    v.callback = R32(desc + 12);
    v.context = R32(desc + 16);
    SetBins(v, R32(desc + 20));
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    W32(out, obj);
    return 0;
}

#define STREAM_REQUIRE() XDK_REQUIRE("dsound", XDK_KEY(CDirectSoundStream_Vtable), XDK_KEY(CDirectSoundStream_Vtable2),                                      XDK_DATA(DSound_StreamVtable), XDK_DATA(DSound_StreamVtable2))
HLE_STDCALL(DirectSoundCreateStream, 2) {
    STREAM_REQUIRE();
    return NewStream(a[0], a[1]);
}
HLE_STDCALL(IDirectSound_CreateSoundStream, 4) {  // this, desc, stream out, unknown
    STREAM_REQUIRE();
    return NewStream(a[1], a[2]);
}

#define STREAM(name, nargs)                                     \
    static uint32_t str_##name(Voice& v, const Args& a);        \
    HLE_STDCALL(CDirectSoundStream_##name, nargs) {             \
        uint32_t r;                                             \
        std::vector<Done> flushed;                              \
        {                                                       \
            DsLock lock; \
            Voice* v = Find(a[0]);                              \
            r = v ? str_##name(*v, a) : 0x80004005u;            \
            flushed.swap((*FlushedList()));                            \
        }                                                       \
        Run(ctx, flushed);                                      \
        return r;                                               \
    }                                                           \
    static uint32_t str_##name(Voice& v, const Args& a)

// the stream's callbacks still queued and those of its flushed packets, in order, go to the calling thread
static void FlushStream(Voice& v) {
    for (size_t k = 0; k < v.packets.size(); k++) Complete(v, v.packets[k], kFlushed);
    v.packets.clear();
    v.pos = 0;
    for (auto it = g_done.begin(); it != g_done.end();) {
        if (it->voice != v.obj) {
            ++it;
            continue;
        }
        (*FlushedList()).push_back(*it);
        it = g_done.erase(it);
    }
    Deactivate(v);
}

STREAM(AddRef, 1) { return uint32_t(++v.refs); }
STREAM(GetVoiceProperties, 2) { return GetVoiceProperties(v, a[1]); }
STREAM(Set3DVoiceData, 2) { return Set3DVoiceData(v, a[1]); }
STREAM(Use3DVoiceData, 2) {
    v.voice3d.enabled = a[1] != 0;
    return 0;
}
HLE_STDCALL(CDirectSoundStream_SetPitch, 2) {
    uint32_t v[1] = {a[1]};
    QueuePend(a[0], kPendPitch, v, 1);
    return 0;
}
STREAM(SetLFO, 2) { return SetLFO(v, a[1]); }
STREAM(SetOutputBuffer, 2) { return 0; }  // the mixer folds every bus to stereo
STREAM(SetFormat, 2) {
    FlushStream(v);
    ReadFormat(v, a[1]);
    v.freq = 0;  // the XDK sets the pitch back to the new format's own rate
    v.pitch_override = false;
    v.ResetAdpcm();
    return 0;
}
STREAM(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    FlushStream(v);
    uint32_t obj = v.obj;
    ForgetPos(obj);
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
STREAM(GetInfo, 2) {
    uint32_t info = a[1];
    W32(info, 0x3);  // fixed sample size, fixed packet alignment
    W32(info + 4, v.tag == 0x69 ? 36u * v.channels : v.align);
    W32(info + 8, 0);
    W32(info + 12, 0);
    return 0;
}
// audio::StreamStatus has the bits, and why a starved stream short of its Discontinuity still says PLAYING. A stream
// releasing under FlushEx keeps PLAYING until its release ends, which XACT and the music player wait on (PC port).
STREAM(GetStatus, 2) {
    audio::StreamState st;
    st.free_slot = v.packets.size() < v.max_packets;
    st.running = v.active && !v.hold;
    st.has_packets = !v.packets.empty();
    st.paused = v.paused || v.sync;
    st.ending = v.ending;
    st.stopping = v.stopping;
    st.starved_ms = v.starved_at < 0 ? -1 : NowMs() - v.starved_at;
    W32(a[1], audio::StreamStatus(st));
    return 0;
}
STREAM(Process, 3) {
    uint32_t in = a[1];
    if (!in) return 0;
    Packet pk = {R32(in), R32(in + 4), R32(in + 8), R32(in + 12), R32(in + 16), 0};
    if (pk.completed_ptr) W32(pk.completed_ptr, 0);
    if (pk.status_ptr) W32(pk.status_ptr, kPacketPending);
    v.packets.push_back(pk);
    if (v.stopping) {
        static unsigned reported = 0;
        if (reported++ < 16) LOGW("dsound: stream %08X refilled while stopping", v.obj);
    }
    // the stream's voice comes on with its first packet, not again at packet boundaries, nor while held
    if (!v.active && !v.hold) {
        v.mod.On();
        // XACT may have started this song on a new stream object while a previous song's stream is starved (ran out
        // of packets during a hitch, waiting for a refill XACT will never send because it moved on). Stop that
        // starved stream so the old song cannot refill and play over this one.
        for (std::map<uint32_t, Voice>::iterator it = g_voices.begin(); it != g_voices.end(); ++it) {
            Voice& ov = it->second;
            if (&ov == &v) continue;
            if (ov.stream && ov.active && !ov.hold && !ov.paused && !ov.sync && !ov.stopping && ov.packets.empty() &&
                ov.starved_at >= 0)
                Deactivate(ov);
        }
    }
    // how long a starved stream was silent: a song that skips is a stereo stream refilled late, the log says when
    if (v.starved_at >= 0) {
        const int64_t silent = NowMs() - v.starved_at;
        static unsigned gaps = 0;
        if (pk.size) {  // MC3 (2026-11-16): every gap of real data, for the mixer's 5 s line (empty packets are silence on purpose)
            g_gap_n++;
            g_gap_ms += silent;
        }
        if (silent >= 20 && gaps++ < 64)
            LOGW("dsound: stream %08X (%u ch) silent %lld ms until its refill", v.obj, unsigned(v.channels), (long long)silent);
    }
    v.active = true;
    v.ending = false;
    v.starved_at = -1;
    return 0;
}
STREAM(Discontinuity, 1) {
    if (!v.packets.empty()) v.ending = true;
    else if (v.active) FlushStream(v);
    return 0;
}
STREAM(Flush, 1) {
    FlushStream(v);
    return 0;
}
// (this, time low, time high, flags), the time is not scheduled. As 0x3C4F37: DSSTREAMFLUSHEX_ENVELOPE2 (4) releases
// the envelopes and keeps the packets playing, they complete as flushed when the release ends; ENVELOPE (2) alone
// flushes them now, with the voice's data gone, and no flag flushes at once. ASYNC (1) only returns early.
STREAM(FlushEx, 4) {
    if (a[3] && !v.active) return 0;
    if (a[3] & 4) {
        v.mod.Release();
        if (!v.mod.Instant()) {
            v.stopping = true;
            return 0;
        }
    }
    FlushStream(v);
    return 0;
}
// As 0x3C5A8C: DSSTREAMPAUSE_RESUME (0), PAUSE (1), SYNCHPLAYBACK (2), PAUSENOACTIVATE (3). PAUSENOACTIVATE on a
// stream that is not running lets its packets queue without starting it; XACT prepares every streamed wave so
// (0x40FED4) and starts it with RESUME when the cue plays (0x40F390). Playing such a stream at once made the next
// song or a cutscene's stream sound over the current one (PC port). On a running stream PAUSENOACTIVATE resumes it.
STREAM(Pause, 2) {
    switch (a[1]) {
    case 0:
        v.paused = v.sync = false;
        if (v.hold) {
            v.hold = false;
            if (v.active) v.mod.On();  // the held packets start the voice now
        }
        return 0;
    case 1:
        v.paused = true;
        v.sync = false;
        return 0;
    case 2:
        v.paused = false;
        v.sync = true;
        return 0;
    case 3:
        if (v.active && !v.hold) v.paused = v.sync = false;
        else v.hold = true;
        return 0;
    }
    return 0x80004005u;
}
STREAM(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
STREAM(SetFrequency, 2) {
    v.freq = a[1];
    v.pitch_override = false;
    return 0;
}
STREAM(SetFilter, 2) { return 0; }
// the IDirectSoundStream thunk at 0x3C22A3 jumps to CDirectSoundStream::SetEG (0x3C1D6F) with the same object
HLE_STDCALL(IDirectSoundStream_SetEG, 2) {
    DsLock lock;
    Voice* v = Find(a[0]);
    return v ? SetEG(*v, a[1]) : 0x80004005u;
}
STREAM(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
HLE_STDCALL(CDirectSoundStream_SetMixBinVolumes, 2) {
    QueueBinVolumes(a[0], a[1]);
    return 0;
}
STREAM(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
STREAM(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
STREAM(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
STREAM(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
STREAM(SetVelocity, 5) { return 0; }
STREAM(SetI3DL2Source, 3) { return 0; }
