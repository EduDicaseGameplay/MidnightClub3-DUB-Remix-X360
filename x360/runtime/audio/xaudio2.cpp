// The DirectSound mixer's output through the Xbox 360's XAudio2: 48 kHz stereo, 16 bit samples in the CPU's own
// byte order, 20 ms buffers with three queued, on a hardware thread of its own (the PC's xaudio2.cpp, ported).
#include "audio/audio.h"

#include <algorithm>
#include <math.h>
#include <vector>

#include "core/log.h"
#include "host.h"

#include <xaudio2.h>

namespace audio {

namespace {

struct VoiceCallback : public IXAudio2VoiceCallback {
    HANDLE buffer_event;
    VoiceCallback() { buffer_event = CreateEvent(NULL, FALSE, FALSE, NULL); }
    ~VoiceCallback() {
        if (buffer_event) CloseHandle(buffer_event);
    }
    void STDMETHODCALLTYPE OnStreamEnd() {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) {
        if (buffer_event) SetEvent(buffer_event);
    }
    void STDMETHODCALLTYPE OnBufferStart(void*) {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) {}
};

IXAudio2* volatile g_engine = NULL;
IXAudio2SourceVoice* g_movie_voice = NULL;

void Run(std::function<void(float*, uint32_t)> render) {
    // MC3 X-19 (2026-11-11): game:\noaudio.txt keeps the DirectSound mixer running at the same pace (the title sees
    // its sounds play and finish) but sends nothing to XAudio2: tells the console's audio output from the rest
    if (GetFileAttributesA("game:\\noaudio.txt") != DWORD(-1)) {
        LOGI("audio: game:\\noaudio.txt, mixer without output");
        const uint32_t kFrames = (kRate * 20) / 1000;
        std::vector<float> mix(kFrames * 2);
        int64_t next = host::NowNs();
        for (;;) {
            render(&mix[0], kFrames);
            next += 20000000;
            int64_t wait = next - host::NowNs();
            if (wait > 0) Sleep(DWORD(wait / 1000000));
            else next = host::NowNs();
        }
    }
    IXAudio2* xa = NULL;
    IXAudio2MasteringVoice* master = NULL;
    IXAudio2SourceVoice* voice = NULL;
    VoiceCallback callback;
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = kRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = kRate * 4;
    // as KI-X360 does on the console, where it plays: XAudio2 on hardware thread 4, the mastering voice's channels
    // and rate the console's defaults, named
    HRESULT hr = XAudio2Create(&xa, 0, XboxThread1);
    if (SUCCEEDED(hr)) hr = xa->CreateMasteringVoice(&master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, 0, NULL);
    if (SUCCEEDED(hr)) hr = xa->CreateSourceVoice(&voice, &wf, 0, 2.0f, &callback, NULL, NULL);
    if (FAILED(hr)) {
        LOGE("audio: XAudio2 unavailable (%08X), no sound", unsigned(hr));
        return;
    }
    voice->Start(0, 0);
    g_engine = xa;
    LOGI("audio: XAudio2 output at %u Hz", kRate);
    const uint32_t kFrames = (kRate * 20) / 1000, kSlots = 12;
    std::vector<float> mix(kFrames * 2);
    std::vector<int16_t> bufs(kSlots * kFrames * 2);
    for (uint32_t n = 0;;) {
        XAUDIO2_VOICE_STATE st;
        voice->GetState(&st);
        // MC3 (2026-11-17): three blocks of 20 ms ahead, not six: the output added 120 ms after the game's frame, and
        // the horns of the traffic were heard late on the console. A block is mixed in ~7 ms there (the mixer's own
        // line in the log); a queue that runs dry is counted below
        if (st.BuffersQueued >= 3) {
            WaitForSingleObject(callback.buffer_event, 25);
            continue;
        }
        static uint32_t dry = 0, dry_told = 0;
        if (n > 50 && st.BuffersQueued == 0) dry++;
        render(&mix[0], kFrames);
        // bring-up: a 440 Hz beep for the first half second proves the output path, apart from the game's mixer
        static uint32_t tone_frames = 0;
        for (uint32_t i = 0; i < kFrames && tone_frames < kRate / 2; i++, tone_frames++) {
            float t = sinf(float(tone_frames) * (2.0f * 3.14159265f * 440.0f / kRate)) * 0.25f;
            mix[i * 2] += t;
            mix[i * 2 + 1] += t;
        }
        // bring-up: the output's peak every 5 s
        static float peak = 0;
        static uint32_t since = 0;
        for (uint32_t i = 0; i < kFrames * 2; i++) peak = std::max(peak, fabsf(mix[i]));
        if ((since += kFrames) >= kRate * 5) {
            LOGI("audio: peak %.3f over the last 5 s; output ran dry %u times", peak, dry - dry_told);
            dry_told = dry;
            peak = 0;
            since = 0;
        }
        int16_t* b = &bufs[(n % kSlots) * kFrames * 2];
        for (uint32_t i = 0; i < kFrames * 2; i++) {
            float v = mix[i] * 32767.0f;
            b[i] = int16_t(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : int(v));
        }
        XAUDIO2_BUFFER xb;
        memset(&xb, 0, sizeof(xb));
        xb.AudioBytes = kFrames * 4;
        xb.pAudioData = reinterpret_cast<const BYTE*>(b);
        voice->SubmitSourceBuffer(&xb);
        n++;
    }
}

}  // namespace

bool MovieOpen(uint32_t rate) {
    MovieStop();
    if (!g_engine) return false;
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = rate * 4;
    if (FAILED(g_engine->CreateSourceVoice(&g_movie_voice, &wf, 0, 2.0f, NULL, NULL, NULL))) return false;
    // the level the game's own player had: output peak 0.57 for a file peaking at 1.0, 0.31 for one at 0.59 (v39's log)
    g_movie_voice->SetVolume(0.5f);
    return true;
}

void MovieSubmit(const void* samples, uint32_t bytes) {
    if (!g_movie_voice) return;
    XAUDIO2_BUFFER xb;
    memset(&xb, 0, sizeof(xb));
    xb.AudioBytes = bytes;
    xb.pAudioData = static_cast<const BYTE*>(samples);
    g_movie_voice->SubmitSourceBuffer(&xb);
}

void MoviePlay(bool play) {
    if (!g_movie_voice) return;
    if (play) g_movie_voice->Start(0, 0);
    else g_movie_voice->Stop(0, 0);
}

uint64_t MoviePlayed() {
    if (!g_movie_voice) return 0;
    XAUDIO2_VOICE_STATE st;
    g_movie_voice->GetState(&st);
    return st.SamplesPlayed;
}

uint32_t MovieQueued() {
    if (!g_movie_voice) return 0;
    XAUDIO2_VOICE_STATE st;
    g_movie_voice->GetState(&st);
    return st.BuffersQueued;
}

void MovieStop() {
    if (!g_movie_voice) return;
    g_movie_voice->Stop(0, 0);
    g_movie_voice->FlushSourceBuffers();
    g_movie_voice->DestroyVoice();
    g_movie_voice = NULL;
}

void StartOutput(std::function<void(float*, uint32_t)> render) {
    // hardware thread 1, core 0 with the light threads: XAudio2 itself runs there too (cores 1 and 2 are the title's
    // thread's and the renderer's, gpu/render_thread.cpp)
    host::Spawn([render] { Run(render); }, "audio", 1);
}

}  // namespace audio
