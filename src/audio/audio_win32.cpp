// audio_win32.cpp — Windows audio backend over WinMM waveOut (system library,
// no third-party dependency). A small ring of buffers is fed by the mixer
// thread; buffer completion is tracked via header flags with an event wakeup.
#include "audio.hpp"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstring>

namespace aw {
namespace {

constexpr int kBufs = 4;
constexpr int kBufFrames = 1024;   // stereo frames per buffer

struct Winmm {
    HWAVEOUT dev = nullptr;
    HANDLE evt = nullptr;
    WAVEHDR hdrs[kBufs]{};
    int16_t data[kBufs][kBufFrames * 2]{};
    int next = 0;

    static Winmm& instance() {
        static Winmm w;
        return w;
    }
};

bool winmmOpen(int rate) {
    Winmm& w = Winmm::instance();
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = (DWORD)rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = (DWORD)rate * 4;

    w.evt = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!w.evt) return false;
    if (waveOutOpen(&w.dev, WAVE_MAPPER, &fmt, (DWORD_PTR)w.evt, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[aw] waveOutOpen failed\n");
        CloseHandle(w.evt);
        w.evt = nullptr;
        return false;
    }
    std::memset(w.data, 0, sizeof(w.data));
    for (int i = 0; i < kBufs; ++i) {
        w.hdrs[i].lpData = (LPSTR)w.data[i];
        w.hdrs[i].dwBufferLength = sizeof(w.data[i]);
        if (waveOutPrepareHeader(w.dev, &w.hdrs[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            fprintf(stderr, "[aw] waveOutPrepareHeader failed\n");
            waveOutClose(w.dev);
            w.dev = nullptr;
            CloseHandle(w.evt);
            w.evt = nullptr;
            return false;
        }
    }
    w.next = 0;
    return true;
}

void winmmWrite(const int16_t* frames, int n) {
    Winmm& w = Winmm::instance();
    const int16_t* p = frames;
    int left = n;
    while (left > 0) {
        WAVEHDR& h = w.hdrs[w.next];
        // Wait until this buffer is no longer queued. The event wakes us per
        // completed buffer; the flag check keeps us correct even if wakeups
        // coalesce (we only ever wait while still queued).
        while (h.dwFlags & WHDR_INQUEUE) {
            DWORD rc = WaitForSingleObject(w.evt, 100);
            if (rc == WAIT_FAILED) return;
        }
        int chunk = left < kBufFrames ? left : kBufFrames;
        std::memcpy(w.data[w.next], p, size_t(chunk) * 4);
        if (chunk < kBufFrames)
            std::memset(w.data[w.next] + chunk * 2, 0, size_t(kBufFrames - chunk) * 4);
        h.dwBufferLength = sizeof(w.data[w.next]);
        if (waveOutWrite(w.dev, &h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            fprintf(stderr, "[aw] waveOutWrite failed\n");
            return;
        }
        w.next = (w.next + 1) % kBufs;
        p += chunk * 2;
        left -= chunk;
    }
}

void winmmClose() {
    Winmm& w = Winmm::instance();
    if (w.dev) {
        waveOutReset(w.dev);
        for (int i = 0; i < kBufs; ++i) waveOutUnprepareHeader(w.dev, &w.hdrs[i], sizeof(WAVEHDR));
        waveOutClose(w.dev);
        w.dev = nullptr;
    }
    if (w.evt) {
        CloseHandle(w.evt);
        w.evt = nullptr;
    }
}

}  // namespace

bool openAudioBackend(AudioBackend& out) {
    if (!winmmOpen(48000)) return false;
    winmmClose();  // probe only; Audio::init reopens for real
    out.open = winmmOpen;
    out.write = winmmWrite;
    out.close = winmmClose;
    out.name = "winmm";
    return true;
}

}  // namespace aw

#endif  // _WIN32
