#include "resampler.h"

#include <algorithm>
#include <cmath>

namespace xf {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kBaseHalfTaps = 16;
constexpr int kMaxHalfTaps = 96;
constexpr double kKaiserBeta = 9.0;
constexpr double kPassband = 0.96;  // cutoff as a fraction of the lower Nyquist

double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  const double q = x * x / 4.0;
  for (int k = 1; k < 64; ++k) {
    term *= q / (static_cast<double>(k) * k);
    sum += term;
    if (term < sum * 1e-12) break;
  }
  return sum;
}

}  // namespace

Resampler::Resampler(int inRate, int outRate, int channels)
    : in_(inRate), out_(outRate), ch_(channels) {
  if (!passthrough()) build();
  reset();
}

void Resampler::build() {
  const double ratio = static_cast<double>(out_) / in_;
  const double scale = std::min(ratio, 1.0);
  const double fc = scale * kPassband;
  half_ = std::min(kMaxHalfTaps, static_cast<int>(std::ceil(kBaseHalfTaps / scale)));
  step_ = static_cast<double>(in_) / out_;

  const int taps = 2 * half_;
  const double i0Beta = besselI0(kKaiserBeta);
  table_.assign(static_cast<size_t>(kPhases + 1) * taps, 0.0f);
  std::vector<double> row(taps);
  for (int p = 0; p <= kPhases; ++p) {
    const double frac = static_cast<double>(p) / kPhases;
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
      const double t = frac + (half_ - 1) - i;  // distance from output point to tap
      const double x = t / half_;
      double w = 0.0;
      if (std::fabs(x) < 1.0) w = besselI0(kKaiserBeta * std::sqrt(1.0 - x * x)) / i0Beta;
      const double arg = kPi * fc * t;
      const double sinc = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(arg) / arg;
      row[i] = fc * sinc * w;
      sum += row[i];
    }
    for (int i = 0; i < taps; ++i) table_[static_cast<size_t>(p) * taps + i] = static_cast<float>(row[i] / sum);
  }
  coef_.resize(taps);
}

void Resampler::reset() {
  buf_.clear();
  if (passthrough()) return;
  // half_ - 1 frames of silent history so the first output lands on input frame 0.
  buf_.assign(static_cast<size_t>(half_ - 1) * ch_, 0.0f);
  pos_ = half_ - 1;
}

void Resampler::process(const float* in, size_t frames, std::vector<float>& out) {
  if (frames == 0) return;
  if (passthrough()) {
    out.insert(out.end(), in, in + frames * ch_);
    return;
  }
  buf_.insert(buf_.end(), in, in + frames * ch_);
  run(out);
}

void Resampler::drain(std::vector<float>& out) {
  if (passthrough()) return;
  buf_.insert(buf_.end(), static_cast<size_t>(half_) * ch_, 0.0f);
  run(out);
  reset();
}

void Resampler::run(std::vector<float>& out) {
  const int taps = 2 * half_;
  const size_t frames = buf_.size() / ch_;
  for (;;) {
    const size_t n = static_cast<size_t>(pos_);
    if (n + half_ >= frames) break;
    const double fp = (pos_ - static_cast<double>(n)) * kPhases;
    const int p = std::min(static_cast<int>(fp), kPhases - 1);
    const float a = static_cast<float>(fp - p);
    const float* r0 = &table_[static_cast<size_t>(p) * taps];
    const float* r1 = r0 + taps;
    for (int i = 0; i < taps; ++i) coef_[i] = r0[i] + a * (r1[i] - r0[i]);

    const float* src = &buf_[(n + 1 - half_) * ch_];
    for (int c = 0; c < ch_; ++c) {
      float acc = 0.0f;
      for (int i = 0; i < taps; ++i) acc += src[static_cast<size_t>(i) * ch_ + c] * coef_[i];
      out.push_back(acc);
    }
    pos_ += step_;
  }
  // Keep only the history the next output still needs.
  const size_t n = static_cast<size_t>(pos_);
  size_t drop = n >= static_cast<size_t>(half_ - 1) ? n - (half_ - 1) : 0;
  drop = std::min(drop, frames);
  if (drop) {
    buf_.erase(buf_.begin(), buf_.begin() + drop * ch_);
    pos_ -= static_cast<double>(drop);
  }
}

}  // namespace xf
