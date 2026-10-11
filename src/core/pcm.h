#pragma once

#include <cstddef>

namespace xf {

// Bits-per-sample values Winamp input plugins hand to output plugins.
bool isSupportedBps(int bps);

// Converts interleaved little-endian integer PCM (8-bit unsigned, 16/24/32-bit
// signed; 24-bit is packed 3 bytes) to float in [-1, 1).
// Returns false for an unsupported bps.
bool pcmToFloat(const void* src, size_t frames, int channels, int bps, float* dst);

// Remaps an interleaved buffer from inCh to outCh channels (WAVE channel order).
// Mono is copied to front left/right, surround is folded down to stereo, and
// extra output channels are left silent. src and dst must not overlap.
void mapChannels(const float* src, int inCh, float* dst, int outCh, size_t frames);

float dbToGain(float db);

}  // namespace xf
