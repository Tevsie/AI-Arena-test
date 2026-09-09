// input.hpp — key/mouse constants shared across backends.
// Key codes follow the X11 keysym convention (ASCII for letters, 0xffXX for
// special keys), which the X11 backend produces natively; the headless backend
// and tests synthesize the same codes.
#pragma once

#include <cstdint>

namespace aw {

// Keyboard (keysym values, compacted into 0..511 for the poll array).
// ASCII keys keep their code (0x00..0x7f); special keys (0xffXX keysyms) are
// folded into 0x100..0x1ff so the per-frame key array stays small.
inline uint32_t mapKeysym(uint32_t ks) { return ks < 0x100 ? ks : (0x100 + (ks & 0xff)); }

constexpr uint32_t KEY_W = 0x77, KEY_A = 0x61, KEY_S = 0x73, KEY_D = 0x64;
constexpr uint32_t KEY_SPACE = 0x20;
constexpr uint32_t KEY_ESC = 0x100 + 0x1b;    // XK_Escape
constexpr uint32_t KEY_SHIFT = 0x100 + 0xe1;  // XK_Shift_L
constexpr uint32_t KEY_CTRL = 0x100 + 0xe3;   // XK_Control_L
constexpr uint32_t KEY_E = 0x65, KEY_Q = 0x71;
constexpr uint32_t KEY_R = 0x72, KEY_F = 0x66, KEY_P = 0x70, KEY_X = 0x78;

// Mouse buttons (index into FrameInput::mousePressed/Released).
constexpr int MB_LEFT = 0;
constexpr int MB_MIDDLE = 1;
constexpr int MB_RIGHT = 2;

}  // namespace aw
