#pragma once

#include <cstdint>

namespace xf {

enum class Curve : int {
  Linear = 0,      // constant gain: in + out == 1
  EqualPower = 1,  // constant power: in^2 + out^2 == 1 (best for unrelated songs)
  SCurve = 2,      // smoothstep: gentle start and end, constant gain
  Logarithmic = 3, // linear in dB over a 60 dB range
};
constexpr int kCurveCount = 4;

const char* curveName(Curve c);

// Rising fade shape: 0 at p == 0, 1 at p == 1. p is clamped to [0, 1].
// A fade-out with the same curve is curveIn(1 - p), so the matching in/out
// pair of a crossfade is complementary for every curve.
float curveIn(Curve c, float p);

// Per-frame gain envelope.
class Fade {
 public:
  // Holds a steady gain.
  void set(float gain);
  // Ramps from `from` to `to` over `frames` frames. frames <= 0 jumps to `to`.
  void start(float from, float to, int64_t frames, Curve curve);
  // Gain for the current frame, then advances one frame.
  float next();
  float gain() const;
  float target() const { return to_; }
  bool active() const { return active_; }
  int64_t remaining() const { return active_ ? len_ - pos_ : 0; }

 private:
  float eval(double p) const;

  bool active_ = false;
  float from_ = 1.0f;
  float to_ = 1.0f;
  int64_t len_ = 0;
  int64_t pos_ = 0;
  Curve curve_ = Curve::EqualPower;
};

}  // namespace xf
