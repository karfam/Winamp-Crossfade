#include "fade.h"

#include <cmath>

namespace xf {

namespace {
constexpr double kHalfPi = 1.57079632679489661923;
constexpr double kLogRangeDb = 60.0;
}  // namespace

const char* curveName(Curve c) {
  switch (c) {
    case Curve::Linear: return "Linear (constant gain)";
    case Curve::EqualPower: return "Equal power (recommended)";
    case Curve::SCurve: return "S-curve";
    case Curve::Logarithmic: return "Logarithmic";
  }
  return "?";
}

float curveIn(Curve c, float pf) {
  double p = pf < 0.0f ? 0.0 : (pf > 1.0f ? 1.0 : pf);
  switch (c) {
    case Curve::Linear:
      return static_cast<float>(p);
    case Curve::EqualPower:
      return static_cast<float>(std::sin(p * kHalfPi));
    case Curve::SCurve:
      return static_cast<float>(p * p * (3.0 - 2.0 * p));
    case Curve::Logarithmic: {
      if (p <= 0.0) return 0.0f;
      const double floor = std::pow(10.0, -kLogRangeDb / 20.0);
      const double g = std::pow(10.0, kLogRangeDb * (p - 1.0) / 20.0);
      return static_cast<float>((g - floor) / (1.0 - floor));
    }
  }
  return static_cast<float>(p);
}

void Fade::set(float gain) {
  active_ = false;
  from_ = to_ = gain;
  len_ = pos_ = 0;
}

void Fade::start(float from, float to, int64_t frames, Curve curve) {
  if (frames <= 0 || from == to) {
    set(to);
    return;
  }
  active_ = true;
  from_ = from;
  to_ = to;
  len_ = frames;
  pos_ = 0;
  curve_ = curve;
}

float Fade::eval(double p) const {
  if (to_ >= from_) return from_ + (to_ - from_) * curveIn(curve_, static_cast<float>(p));
  return to_ + (from_ - to_) * curveIn(curve_, static_cast<float>(1.0 - p));
}

float Fade::next() {
  if (!active_) return to_;
  const float g = eval(static_cast<double>(pos_) / static_cast<double>(len_));
  if (++pos_ >= len_) active_ = false;
  return g;
}

float Fade::gain() const {
  if (!active_) return to_;
  return eval(static_cast<double>(pos_) / static_cast<double>(len_));
}

}  // namespace xf
