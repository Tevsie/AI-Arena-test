// audio_linux.cpp — Linux audio backends, resolved at runtime via dlopen so the
// engine keeps its zero-link-dependency rule (libc/libm/libdl only).
// Tries PulseAudio-simple first (covers PipeWire-pulse too), then raw ALSA.
#include "audio.hpp"

#if !defined(_WIN32)

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>

namespace aw {
namespace {

// ---------------------------------------------------------------------------
// PulseAudio-simple (minimal ABI mirror)
// ---------------------------------------------------------------------------
struct PaSimple;
struct PaSampleSpec {
    int format;          // pa_sample_format_t (PA_SAMPLE_S16LE = 3)
    uint32_t rate;
    uint8_t channels;
    uint8_t pad[3];
};

extern "C" {
using PaNewFn = PaSimple* (*)(const char*, const char*, int, const char*, const char*,
                              const PaSampleSpec*, const void*, const void*, int*);
using PaWriteFn = int (*)(PaSimple*, const void*, size_t, int*);
using PaDrainFn = int (*)(PaSimple*, int*);
using PaFreeFn = void (*)(PaSimple*);
}

struct Pulse {
    void* lib = nullptr;
    PaNewFn open = nullptr;
    PaWriteFn write = nullptr;
    PaDrainFn drain = nullptr;
    PaFreeFn free = nullptr;
    PaSimple* stream = nullptr;

    static Pulse& instance() {
        static Pulse p;
        return p;
    }

    bool load() {
        if (lib) return true;
        lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
        if (!lib) lib = dlopen("libpulse-simple.so", RTLD_LAZY);
        if (!lib) return false;
        open = reinterpret_cast<PaNewFn>(dlsym(lib, "pa_simple_new"));
        write = reinterpret_cast<PaWriteFn>(dlsym(lib, "pa_simple_write"));
        drain = reinterpret_cast<PaDrainFn>(dlsym(lib, "pa_simple_drain"));
        free = reinterpret_cast<PaFreeFn>(dlsym(lib, "pa_simple_free"));
        return open && write && drain && free;
    }
};

bool pulseOpen(int rate) {
    Pulse& p = Pulse::instance();
    if (!p.load()) return false;
    PaSampleSpec ss{3 /*S16LE*/, uint32_t(rate), 2, {0, 0, 0}};
    int err = 0;
    p.stream = p.open(nullptr, "atw", 1 /*PLAYBACK*/, nullptr, "sfx", &ss, nullptr, nullptr, &err);
    if (!p.stream) {
        fprintf(stderr, "[aw] pa_simple_new failed (%d)\n", err);
        return false;
    }
    return true;
}

void pulseWrite(const int16_t* frames, int n) {
    Pulse& p = Pulse::instance();
    int err = 0;
    if (p.write(p.stream, frames, size_t(n) * 4, &err) < 0)
        fprintf(stderr, "[aw] pa_simple_write failed (%d)\n", err);
}

void pulseClose() {
    Pulse& p = Pulse::instance();
    if (p.stream) {
        int err = 0;
        p.drain(p.stream, &err);
        p.free(p.stream);
        p.stream = nullptr;
    }
}

// ---------------------------------------------------------------------------
// ALSA (minimal ABI mirror)
// ---------------------------------------------------------------------------
struct SndPcm;
struct SndPcmHwParams;

extern "C" {
using SndOpenFn = int (*)(SndPcm**, const char*, int, int);
using SndCloseFn = int (*)(SndPcm*);
using SndHwMallocFn = int (*)(SndPcmHwParams**);
using SndHwFreeFn = void (*)(SndPcmHwParams*);
using SndHwAnyFn = int (*)(SndPcm*, SndPcmHwParams*);
using SndHwAccessFn = int (*)(SndPcm*, SndPcmHwParams*, int);
using SndHwFormatFn = int (*)(SndPcm*, SndPcmHwParams*, int);
using SndHwRateNearFn = int (*)(SndPcm*, SndPcmHwParams*, unsigned*, int*);
using SndHwChannelsFn = int (*)(SndPcm*, SndPcmHwParams*, unsigned);
using SndHwApplyFn = int (*)(SndPcm*, SndPcmHwParams*);
using SndPrepareFn = int (*)(SndPcm*);
using SndWriteiFn = long (*)(SndPcm*, const void*, unsigned long);
using SndRecoverFn = int (*)(SndPcm*, int, int);
using SndDrainFn = int (*)(SndPcm*);
}

struct Alsa {
    void* lib = nullptr;
    SndOpenFn open = nullptr;
    SndCloseFn close = nullptr;
    SndHwMallocFn hwMalloc = nullptr;
    SndHwFreeFn hwFree = nullptr;
    SndHwAnyFn hwAny = nullptr;
    SndHwAccessFn hwAccess = nullptr;
    SndHwFormatFn hwFormat = nullptr;
    SndHwRateNearFn hwRateNear = nullptr;
    SndHwChannelsFn hwChannels = nullptr;
    SndHwApplyFn hwApply = nullptr;
    SndPrepareFn prepare = nullptr;
    SndWriteiFn writei = nullptr;
    SndRecoverFn recover = nullptr;
    SndDrainFn drain = nullptr;
    SndPcm* pcm = nullptr;

    static Alsa& instance() {
        static Alsa a;
        return a;
    }

    bool load() {
        if (lib) return true;
        lib = dlopen("libasound.so.2", RTLD_LAZY);
        if (!lib) lib = dlopen("libasound.so", RTLD_LAZY);
        if (!lib) return false;
#define ALSASYM(field, sym) field = reinterpret_cast<decltype(field)>(dlsym(lib, sym))
        ALSASYM(open, "snd_pcm_open");
        ALSASYM(close, "snd_pcm_close");
        ALSASYM(hwMalloc, "snd_pcm_hw_params_malloc");
        ALSASYM(hwFree, "snd_pcm_hw_params_free");
        ALSASYM(hwAny, "snd_pcm_hw_params_any");
        ALSASYM(hwAccess, "snd_pcm_hw_params_set_access");
        ALSASYM(hwFormat, "snd_pcm_hw_params_set_format");
        ALSASYM(hwRateNear, "snd_pcm_hw_params_set_rate_near");
        ALSASYM(hwChannels, "snd_pcm_hw_params_set_channels");
        ALSASYM(hwApply, "snd_pcm_hw_params");
        ALSASYM(prepare, "snd_pcm_prepare");
        ALSASYM(writei, "snd_pcm_writei");
        ALSASYM(recover, "snd_pcm_recover");
        ALSASYM(drain, "snd_pcm_drain");
#undef ALSASYM
        return open && close && hwMalloc && hwFree && hwAny && hwAccess && hwFormat &&
               hwRateNear && hwChannels && hwApply && prepare && writei && recover && drain;
    }
};

bool alsaOpen(int rate) {
    Alsa& a = Alsa::instance();
    if (!a.load()) return false;
    if (a.open(&a.pcm, "default", 0 /*PLAYBACK*/, 0) < 0) {
        fprintf(stderr, "[aw] snd_pcm_open(default) failed\n");
        return false;
    }
    SndPcmHwParams* hw = nullptr;
    bool ok = false;
    if (a.hwMalloc(&hw) == 0 && a.hwAny(a.pcm, hw) == 0 &&
        a.hwAccess(a.pcm, hw, 3 /*RW_INTERLEAVED*/) == 0 &&
        a.hwFormat(a.pcm, hw, 2 /*S16_LE*/) == 0) {
        unsigned r = unsigned(rate);
        int dir = 0;
        if (a.hwRateNear(a.pcm, hw, &r, &dir) == 0 &&
            a.hwChannels(a.pcm, hw, 2) == 0 && a.hwApply(a.pcm, hw) == 0 &&
            a.prepare(a.pcm) == 0)
            ok = true;
    }
    if (hw) a.hwFree(hw);
    if (!ok) {
        fprintf(stderr, "[aw] ALSA hw setup failed\n");
        a.close(a.pcm);
        a.pcm = nullptr;
        return false;
    }
    return true;
}

void alsaWrite(const int16_t* frames, int n) {
    Alsa& a = Alsa::instance();
    const int16_t* p = frames;
    int left = n;
    while (left > 0) {
        long rc = a.writei(a.pcm, p, (unsigned long)left);
        if (rc < 0) {  // xrun/suspend: recover and retry
            if (a.recover(a.pcm, int(rc), 0) < 0) {
                fprintf(stderr, "[aw] ALSA write error %ld\n", rc);
                return;
            }
            continue;
        }
        if (rc == 0) break;
        p += rc * 2;
        left -= int(rc);
    }
}

void alsaClose() {
    Alsa& a = Alsa::instance();
    if (a.pcm) {
        a.drain(a.pcm);
        a.close(a.pcm);
        a.pcm = nullptr;
    }
}

}  // namespace

bool openAudioBackend(AudioBackend& out) {
    if (pulseOpen(48000)) {
        // Probed OK; close the probe stream — Audio::init reopens for real.
        pulseClose();
        out.open = pulseOpen;
        out.write = pulseWrite;
        out.close = pulseClose;
        out.name = "pulseaudio";
        return true;
    }
    if (alsaOpen(48000)) {
        alsaClose();
        out.open = alsaOpen;
        out.write = alsaWrite;
        out.close = alsaClose;
        out.name = "alsa";
        return true;
    }
    return false;
}

}  // namespace aw

#endif  // !_WIN32
