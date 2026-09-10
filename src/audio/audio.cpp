// audio.cpp — mixer thread + procedural sound synthesis.
#include "audio.hpp"

#include <cmath>

namespace aw {
namespace {

// Effect durations in seconds.
float sfxDuration(Sfx id) {
    switch (id) {
        case Sfx::Click:   return 0.05f;
        case Sfx::Pull:    return 0.22f;
        case Sfx::Push:    return 0.22f;
        case Sfx::Done:    return 0.15f;
        case Sfx::Jump:    return 0.18f;
        case Sfx::Land:    return 0.25f;
        case Sfx::Restart: return 0.40f;
    }
    return 0.1f;
}

constexpr float kTau = 6.28318530717958647692f;

// Deterministic noise in [-1, 1] (xorshift).
float noise(uint32_t& rng) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return float(int32_t(rng)) / 2147483648.0f;
}

// Sine with a linear frequency sweep f0 -> f1 over T seconds (exact phase).
float sweep(float t, float f0, float f1, float T) {
    return std::sin(kTau * (f0 * t + (f1 - f0) * t * t / (2.0f * T)));
}

float attack(float t, float a) { return t < a ? t / a : 1.0f; }

// One mono sample of an effect at time t (seconds since trigger).
float synth(Sfx id, float t, uint32_t& rng) {
    switch (id) {
        case Sfx::Click:
            return std::sin(kTau * 2000.0f * t) * std::exp(-t * 140.0f) * 0.7f +
                   noise(rng) * 0.35f * std::exp(-t * 220.0f);
        case Sfx::Pull:
            return sweep(t, 280.0f, 660.0f, 0.22f) * attack(t, 0.01f) *
                   std::exp(-t * 8.0f) * 0.6f + noise(rng) * 0.06f;
        case Sfx::Push:
            return sweep(t, 660.0f, 280.0f, 0.22f) * attack(t, 0.01f) *
                   std::exp(-t * 8.0f) * 0.6f + noise(rng) * 0.06f;
        case Sfx::Done:
            return std::sin(kTau * 880.0f * t) * std::exp(-t * 25.0f) * 0.5f +
                   std::sin(kTau * 1318.0f * t) * std::exp(-t * 30.0f) * 0.25f;
        case Sfx::Jump:
            return sweep(t, 350.0f, 720.0f, 0.18f) * attack(t, 0.02f) *
                   std::exp(-t * 10.0f) * 0.45f;
        case Sfx::Land:
            return std::sin(kTau * 82.0f * t) * std::exp(-t * 22.0f) * 0.8f +
                   noise(rng) * 0.4f * std::exp(-t * 45.0f);
        case Sfx::Restart:
            return sweep(t, 240.0f, 1150.0f, 0.40f) * attack(t, 0.02f) *
                   std::exp(-t * 6.0f) * 0.5f +
                   sweep(t, 480.0f, 2300.0f, 0.40f) * std::exp(-t * 8.0f) * 0.15f;
    }
    return 0.0f;
}

}  // namespace

bool Audio::init(bool enabled) {
    shutdown();
    if (!enabled) {
        backend_ = AudioBackend{};
        backend_.name = "disabled";
        return true;
    }
    if (!openAudioBackend(backend_) || !backend_.open || !backend_.open(kRate)) {
        backend_ = AudioBackend{};
        backend_.name = "none";
        return true;   // silent mode: the game runs fine without audio
    }
    stop_.store(false);
    try {
        thread_ = std::thread(&Audio::threadMain, this);
    } catch (...) {
        if (backend_.close) backend_.close();
        backend_ = AudioBackend{};
        backend_.name = "none";
        return false;
    }
    running_ = true;
    return true;
}

void Audio::shutdown() {
    if (running_) {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        if (backend_.close) backend_.close();
        running_ = false;
    }
    backend_ = AudioBackend{};
    backend_.name = "none";
}

void Audio::setVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    volume_.store(v);
}

void Audio::play(Sfx id) {
    if (!running_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Voice* slot = nullptr;
    for (int i = 0; i < kVoices; ++i) {
        if (!voices_[i].active) { slot = &voices_[i]; break; }
    }
    if (!slot) {  // all busy: steal round-robin
        slot = &voices_[steal_ % kVoices];
        steal_++;
    }
    static uint32_t seed = 0x243F6A88u;
    seed += 0x9E3779B9u;
    slot->active = true;
    slot->id = id;
    slot->t = 0.0f;
    slot->rng = seed ? seed : 1u;
}

void Audio::threadMain() {
    static int16_t buf[kBlock * 2];
    const float dt = 1.0f / float(kRate);
    while (!stop_.load()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            float vol = volume_.load();
            for (int i = 0; i < kBlock; ++i) {
                float m = 0.0f;
                for (int v = 0; v < kVoices; ++v) {
                    Voice& vc = voices_[v];
                    if (!vc.active) continue;
                    m += synth(vc.id, vc.t, vc.rng);
                    vc.t += dt;
                    if (vc.t >= sfxDuration(vc.id)) vc.active = false;
                }
                // Gentle soft-clip so stacked effects don't harshly clip.
                float x = m * vol;
                x /= (1.0f + 0.25f * std::fabs(x));
                int s = int(x * 32767.0f);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                buf[i * 2] = int16_t(s);
                buf[i * 2 + 1] = int16_t(s);
            }
        }
        backend_.write(buf, kBlock);
    }
}

}  // namespace aw
