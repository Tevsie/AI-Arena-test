// audio.hpp — minimal procedural sound engine (zero third-party dependencies).
//
// A mixer thread renders up to 8 simultaneous one-shot synthesized effects
// (UI clicks, brick pull/push, jump/land) to stereo 48 kHz int16 and feeds a
// platform backend: WinMM on Windows, PulseAudio (dlopen) with an ALSA
// (dlopen) fallback on Linux, silence when nothing is available (headless,
// CI, missing device). `play()` is thread-safe fire-and-forget; the master
// volume (settings bar) scales the mixer output live.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace aw {

enum class Sfx : uint8_t {
    Click,    // UI tick / button
    Pull,     // brick lerp started outward
    Push,     // brick lerp started inward
    Done,     // brick lerp completed
    Jump,
    Land,
    Restart,
};

// Platform backend: blocking stereo-int16 sink owned by the mixer thread.
struct AudioBackend {
    bool (*open)(int sampleRate) = nullptr;
    void (*write)(const int16_t* frames, int frameCount) = nullptr;
    void (*close)() = nullptr;
    const char* name = "none";
};

// Implemented per-platform (audio_win32.cpp / audio_linux.cpp). Returns false
// when no usable device/API exists (caller then runs silent).
bool openAudioBackend(AudioBackend& out);

class Audio {
public:
    Audio() = default;
    ~Audio() { shutdown(); }
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // `enabled=false` (headless/tests) selects silent mode without a thread.
    // Always safe to call; returns true unless the mixer thread failed.
    bool init(bool enabled);
    void shutdown();

    void setVolume(float v);            // master gain 0..1 (clamped)
    float volume() const { return volume_.load(); }
    void play(Sfx id);                  // thread-safe, never blocks the caller
    bool ready() const { return running_; }
    const char* backendName() const { return backend_.name; }

private:
    struct Voice {
        bool active = false;
        Sfx id = Sfx::Click;
        float t = 0.0f;        // seconds since trigger
        uint32_t rng = 1;      // per-voice noise state
    };

    void threadMain();

    static constexpr int kRate = 48000;
    static constexpr int kBlock = 512;   // stereo frames per mixer block
    static constexpr int kVoices = 8;

    Voice voices_[kVoices]{};
    int steal_ = 0;                      // round-robin slot when all busy
    std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<float> volume_{0.8f};
    AudioBackend backend_;
    bool running_ = false;               // mixer thread alive (init/shutdown thread only)
};

}  // namespace aw
