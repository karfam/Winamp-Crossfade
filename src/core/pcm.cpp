#include "pcm.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace xf {

bool isSupportedBps(int bps) { return bps == 8 || bps == 16 || bps == 24 || bps == 32; }

bool pcmToFloat(const void* src, size_t frames, int channels, int bps, float* dst) {
  const size_t n = frames * static_cast<size_t>(channels);
  const uint8_t* p = static_cast<const uint8_t*>(src);
  switch (bps) {
    case 8:
      for (size_t i = 0; i < n; ++i) dst[i] = (static_cast<int>(p[i]) - 128) * (1.0f / 128.0f);
      return true;
    case 16:
      for (size_t i = 0; i < n; ++i) {
        int16_t v;
        std::memcpy(&v, p + i * 2, 2);
        dst[i] = v * (1.0f / 32768.0f);
      }
      return true;
    case 24:
      for (size_t i = 0; i < n; ++i) {
        const uint8_t* s = p + i * 3;
        int32_t v = static_cast<int32_t>(s[0] | (s[1] << 8) | (static_cast<uint32_t>(s[2]) << 16));
        if (v & 0x800000) v -= 0x1000000;
        dst[i] = static_cast<float>(v) * (1.0f / 8388608.0f);
      }
      return true;
    case 32:
      for (size_t i = 0; i < n; ++i) {
        int32_t v;
        std::memcpy(&v, p + i * 4, 4);
        dst[i] = static_cast<float>(static_cast<double>(v) * (1.0 / 2147483648.0));
      }
      return true;
    default:
      return false;
  }
}

namespace {

// Fold-down coefficients for multichannel -> stereo, indexed by WAVE default
// channel order for each channel count (FL FR FC LFE BL BR SL SR).
constexpr float kC = 0.70710678f;
struct Fold {
  float l, r;
};
const Fold kFold3[] = {{1, 0}, {0, 1}, {kC, kC}};                                  // FL FR FC
const Fold kFold4[] = {{1, 0}, {0, 1}, {kC, 0}, {0, kC}};                          // FL FR BL BR
const Fold kFold5[] = {{1, 0}, {0, 1}, {kC, kC}, {kC, 0}, {0, kC}};                // FL FR FC BL BR
const Fold kFold6[] = {{1, 0}, {0, 1}, {kC, kC}, {0, 0}, {kC, 0}, {0, kC}};        // 5.1
const Fold kFold7[] = {{1, 0}, {0, 1}, {kC, kC}, {0, 0}, {0.5f, 0.5f}, {kC, 0}, {0, kC}};  // 6.1
const Fold kFold8[] = {{1, 0}, {0, 1}, {kC, kC}, {0, 0}, {kC, 0}, {0, kC}, {kC, 0}, {0, kC}};  // 7.1

const Fold* foldTable(int ch) {
  switch (ch) {
    case 3: return kFold3;
    case 4: return kFold4;
    case 5: return kFold5;
    case 6: return kFold6;
    case 7: return kFold7;
    case 8: return kFold8;
    default: return nullptr;
  }
}

}  // namespace

void mapChannels(const float* src, int inCh, float* dst, int outCh, size_t frames) {
  if (inCh == outCh) {
    std::memcpy(dst, src, frames * static_cast<size_t>(inCh) * sizeof(float));
    return;
  }
  for (size_t f = 0; f < frames; ++f) {
    const float* s = src + f * static_cast<size_t>(inCh);
    float* d = dst + f * static_cast<size_t>(outCh);
    for (int c = 0; c < outCh; ++c) d[c] = 0.0f;

    if (outCh == 1) {
      float sum = 0.0f;
      for (int c = 0; c < inCh; ++c) sum += s[c];
      d[0] = sum / static_cast<float>(inCh);
    } else if (inCh == 1) {
      d[0] = d[1] = s[0];
    } else if (inCh == 2) {
      d[0] = s[0];
      d[1] = s[1];
    } else if (outCh == 2) {
      const Fold* t = foldTable(inCh);
      float l = 0.0f, r = 0.0f, norm = 0.0f;
      for (int c = 0; c < inCh; ++c) {
        l += s[c] * t[c].l;
        r += s[c] * t[c].r;
        norm += t[c].l;
      }
      d[0] = l / norm;
      d[1] = r / norm;
    } else {
      const int n = inCh < outCh ? inCh : outCh;
      for (int c = 0; c < n; ++c) d[c] = s[c];
    }
  }
}

float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

}  // namespace xf
