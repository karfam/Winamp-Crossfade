#pragma once

// Synthetic test signals (mono float).

#include <cmath>
#include <cstdint>
#include <vector>

namespace xft {

inline float rnd(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return static_cast<float>((state >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

// Kick drum on every beat (first beat at phaseSec), optional snare on the
// off-beats and hats on eighths, over background noise.
inline std::vector<float> drumLoop(int rate, double bpm, double seconds, double phaseSec, float noiseLevel,
                                   uint32_t seed, bool offbeatSnare = false, bool hats = false) {
  const double pi = 3.14159265358979323846;
  const size_t n = static_cast<size_t>(seconds * rate);
  std::vector<float> x(n, 0.0f);
  uint32_t st = seed;
  for (size_t i = 0; i < n; ++i) x[i] = noiseLevel * rnd(st);
  const double period = 60.0 / bpm;
  auto addHit = [&](double t, int kind) {
    const size_t s = static_cast<size_t>(t * rate);
    const size_t len = static_cast<size_t>(0.25 * rate);
    for (size_t k = 0; k < len && s + k < n; ++k) {
      const double tt = static_cast<double>(k) / rate;
      double v = 0;
      if (kind == 0) v = 0.6 * std::sin(2 * pi * (55 + 60 * std::exp(-tt * 30)) * tt) * std::exp(-tt * 12);
      else if (kind == 1) v = 0.35 * rnd(st) * std::exp(-tt * 25);
      else v = 0.08 * rnd(st) * std::exp(-tt * 80);
      x[s + k] += static_cast<float>(v);
    }
  };
  for (double t = phaseSec; t < seconds; t += period) {
    addHit(t, 0);
    if (offbeatSnare) addHit(t + period / 2, 1);
    if (hats) {
      addHit(t + period / 4, 2);
      addHit(t + 3 * period / 4, 2);
    }
  }
  return x;
}

inline std::vector<float> noise(int rate, double seconds, float level, uint32_t seed) {
  std::vector<float> x(static_cast<size_t>(seconds * rate));
  for (auto& v : x) v = level * rnd(seed);
  return x;
}

inline std::vector<float> chord(int rate, double seconds, float level) {
  const double pi = 3.14159265358979323846;
  std::vector<float> x(static_cast<size_t>(seconds * rate));
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    x[i] = static_cast<float>(level / 3 * (std::sin(2 * pi * 220 * t) + std::sin(2 * pi * 277.2 * t) + std::sin(2 * pi * 329.6 * t)));
  }
  return x;
}

// Multiplies x by a gain ramp in dB (linear in dB) from fromSec to toSec;
// after toSec the final gain holds.
inline void applyDbRamp(std::vector<float>& x, int rate, double fromSec, double toSec, double fromDb, double toDb) {
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    if (t < fromSec) continue;
    const double p = toSec > fromSec ? std::min(1.0, (t - fromSec) / (toSec - fromSec)) : 1.0;
    x[i] *= static_cast<float>(std::pow(10.0, (fromDb + (toDb - fromDb) * p) / 20.0));
  }
}

}  // namespace xft
