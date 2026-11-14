// Xbox 360 build of the PC port's runtime/audio/voice3d.h (MC3, 2026-11-11), written for VS2010.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace audio {

inline int64_t ClampI64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

// XDK DS3DVOICEDATA, recovered from the title's Set3DVoiceData implementation.
// The mask updates groups independently; values absent from an update retain their previous state.
struct Voice3DData {
    uint32_t words[14];
    bool enabled;
    Voice3DData() : enabled(false) { memset(words, 0, sizeof(words)); }

    void Set(const uint32_t* update) {
        static const uint8_t first[] = {1, 2, 3, 5, 7, 9, 10, 12};
        static const uint8_t count[] = {1, 1, 2, 2, 2, 1, 2, 2};
        words[0] = update[0];
        for (unsigned group = 0; group < 8; ++group)
            if (update[0] & (1u << group))
                for (unsigned i = 0; i < count[group]; ++i) words[first[group] + i] = update[first[group] + i];
    }

    int32_t DistanceVolume() const { return static_cast<int32_t>(words[1]); }
    int32_t ConeVolume() const { return static_cast<int32_t>(words[2]); }
    int32_t DopplerPitch() const { return static_cast<int32_t>(words[9]); }
    int64_t Value(unsigned i) const { return int64_t(int32_t(words[i])); }
    int32_t Attenuation(uint32_t bin) const {
        if (!enabled) return 0;
        int64_t mb = ClampI64(int64_t(DistanceVolume()) + ConeVolume(), -10000, 0);
        if (bin == 6 || bin == 7) mb += Value(7) + Value(3) + Value(5);
        else if (bin == 8 || bin == 9) mb += Value(7) + Value(4);
        else if (bin == 2) mb += Value(7) + Value(3) + Value(6);
        else if (bin == 10) mb += Value(8);
        return int32_t(ClampI64(mb, -10000, 0));
    }
    float Gain(uint32_t bin) const {
        if (!enabled) return 1.0f;
        int32_t mb = Attenuation(bin);
        return mb <= -10000 ? 0.0f : powf(10.0f, float(mb) / 2000.0f);
    }
    double PitchRatio(double base_pitch) const {
        // Add Doppler before the hardware's final clamp; opposite pitch shifts may cancel.
        double pitch = base_pitch + (enabled ? DopplerPitch() : 0);
        pitch = pitch < -32767.0 ? -32767.0 : pitch > 8191.0 ? 8191.0 : pitch;
        return pow(2.0, pitch / 4096.0);
    }
};

}  // namespace audio
