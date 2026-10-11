#include "analysis.h"

#include <algorithm>
#include <cmath>

namespace xf {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Beat detection tuning.
constexpr double kMinBpm = 60.0;
constexpr double kMaxBpm = 200.0;
constexpr double kPreferredBpm = 120.0;  // tempo prior centre
constexpr double kPriorOctaves = 0.9;    // tempo prior width
constexpr double kMinSeconds = 3.0;
constexpr float kMinPeak = 0.2f;         // normalised autocorrelation at the beat lag
constexpr float kMinContrast = 0.1f;     // ...relative to the average over all lags
constexpr double kMinBeatStrength = 1.0; // mean log-energy rise on the beats, summed over bands

// Fade-out detection tuning.
constexpr double kFadeMinTrackSeconds = 6.0;
constexpr double kFadeSmoothSeconds = 0.5;
constexpr double kFadeRefSeconds = 30.0;
constexpr double kFadeRefPercentile = 0.9;
constexpr double kFadeBodyDb = 4.0;       // still "full level" within this of the body level
constexpr double kFadeMinSeconds = 3.0;
constexpr double kFadeMinDepthDb = 15.0;  // how far below body level the track must end
constexpr double kFadeMinSlopeDbPerSec = -1.0;
constexpr double kFadeMaxCorrelation = -0.85;  // dB must fall steadily with time
constexpr double kQuietTrackDb = -50.0;

}  // namespace

BeatGrid detectBeats(const float* x, size_t n, int rate) {
  BeatGrid g;
  if (!x || rate <= 0) return g;
  const int hop = std::max(1, rate / 100);  // 10 ms onset resolution
  const double hopsPerSec = static_cast<double>(rate) / hop;
  const size_t hops = n / hop;
  if (hops < static_cast<size_t>(kMinSeconds * hopsPerSec)) return g;

  // Band energies per hop: lows (kick), mids (snare, voices), highs (hats).
  const double aLow = 1.0 - std::exp(-2.0 * kPi * 150.0 / rate);
  const double aMid = 1.0 - std::exp(-2.0 * kPi * 2500.0 / rate);
  std::vector<double> band[3];
  for (auto& b : band) b.assign(hops, 0.0);
  double lp1 = 0.0, lp2 = 0.0;
  for (size_t h = 0; h < hops; ++h) {
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < hop; ++i) {
      const double v = x[h * hop + i];
      lp1 += aLow * (v - lp1);
      lp2 += aMid * (v - lp2);
      const double mid = lp2 - lp1, high = v - lp2;
      s0 += lp1 * lp1;
      s1 += mid * mid;
      s2 += high * high;
    }
    band[0][h] = s0 / hop;
    band[1][h] = s1 / hop;
    band[2][h] = s2 / hop;
  }

  // Average over 3 hops (30 ms) so fast amplitude beating between sustained
  // tones does not look like attacks.
  for (auto& b : band) {
    std::vector<double> sm(hops);
    for (size_t h = 0; h < hops; ++h) {
      const size_t lo = h >= 2 ? h - 2 : 0;
      double s = 0;
      for (size_t k = lo; k <= h; ++k) s += b[k];
      sm[h] = s / static_cast<double>(h - lo + 1);
    }
    b.swap(sm);
  }

  // Onset strength: rectified rise in log energy, summed over bands. The
  // floor (-30 dB below the overall mean level) keeps near-silent bands and
  // passages from producing large log differences.
  double total = 0;
  for (auto& b : band)
    for (double v : b) total += v;
  const double floor = total / static_cast<double>(hops) * 1e-3 + 1e-12;
  std::vector<double> onset(hops, 0.0);
  for (auto& b : band) {
    double prev = std::log(b[0] + floor);
    for (size_t h = 1; h < hops; ++h) {
      const double cur = std::log(b[h] + floor);
      if (cur > prev) onset[h] += cur - prev;
      prev = cur;
    }
  }

  // Remove the slowly varying part (local mean over +-0.25 s) and rectify.
  const size_t w = static_cast<size_t>(std::lround(0.25 * hopsPerSec));
  std::vector<double> prefix(hops + 1, 0.0);
  for (size_t h = 0; h < hops; ++h) prefix[h + 1] = prefix[h] + onset[h];
  std::vector<double> o(hops);
  for (size_t h = 0; h < hops; ++h) {
    const size_t lo = h >= w ? h - w : 0, hi = std::min(hops, h + w + 1);
    o[h] = std::max(0.0, onset[h] - (prefix[hi] - prefix[lo]) / static_cast<double>(hi - lo));
  }

  // Normalised autocorrelation of the zero-mean envelope.
  double mean = 0;
  for (double v : o) mean += v;
  mean /= static_cast<double>(hops);
  std::vector<double> z(hops);
  double energy = 0;
  for (size_t h = 0; h < hops; ++h) {
    z[h] = o[h] - mean;
    energy += z[h] * z[h];
  }
  if (energy <= 0) return g;
  energy /= static_cast<double>(hops);

  const size_t minLag = static_cast<size_t>(std::floor(hopsPerSec * 60.0 / kMaxBpm));
  const size_t maxLag = static_cast<size_t>(std::ceil(hopsPerSec * 60.0 / kMinBpm));
  const size_t acLen = std::min(2 * maxLag + 2, hops - 1);
  if (maxLag + 2 >= acLen) return g;
  std::vector<double> ac(acLen, 0.0);
  for (size_t l = 1; l < acLen; ++l) {
    double s = 0;
    for (size_t k = 0; k + l < hops; ++k) s += z[k] * z[k + l];
    ac[l] = s / static_cast<double>(hops - l) / energy;
  }

  // Score each candidate period: its own peak plus its double (beats repeat),
  // weighted by a broad prior around 120 BPM to settle octave ambiguity.
  size_t best = 0;
  double bestScore = 0, acMean = 0;
  for (size_t l = minLag; l <= maxLag; ++l) {
    acMean += ac[l];
    double s = ac[l] + (2 * l < acLen ? 0.5 * ac[2 * l] : 0.0);
    const double bpm = 60.0 * hopsPerSec / static_cast<double>(l);
    const double oct = std::log2(bpm / kPreferredBpm) / kPriorOctaves;
    s = std::max(0.0, s) * std::exp(-0.5 * oct * oct);
    if (s > bestScore) {
      bestScore = s;
      best = l;
    }
  }
  acMean /= static_cast<double>(maxLag - minLag + 1);
  if (best == 0) return g;

  g.confidence = static_cast<float>(ac[best]);
  if (ac[best] < kMinPeak || ac[best] - acMean < kMinContrast) return g;

  // Sub-hop period via a parabola through the peak.
  double period = static_cast<double>(best);
  {
    const double a = ac[best - 1], b = ac[best], c = ac[best + 1];
    const double den = a - 2 * b + c;
    if (den < 0) period += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
  }

  // Phase: the offset whose beat comb collects the most onset strength.
  const size_t steps = static_cast<size_t>(std::ceil(period));
  std::vector<double> comb(steps, 0.0);
  for (size_t p = 0; p < steps; ++p) {
    double s = 0;
    int count = 0;
    for (double t = static_cast<double>(p); t < static_cast<double>(hops); t += period, ++count)
      s += o[static_cast<size_t>(std::lround(t)) < hops ? static_cast<size_t>(std::lround(t)) : hops - 1];
    comb[p] = count ? s / count : 0.0;
  }
  const size_t bp = static_cast<size_t>(std::max_element(comb.begin(), comb.end()) - comb.begin());
  // A periodic ripple (e.g. sustained tones beating) correlates well but has
  // no real attacks; require audible onsets on the beats.
  if (comb[bp] < kMinBeatStrength) return g;
  double phase = static_cast<double>(bp);
  {
    const double a = comb[(bp + steps - 1) % steps], b = comb[bp], c = comb[(bp + 1) % steps];
    const double den = a - 2 * b + c;
    if (den < 0) phase += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    if (phase < 0) phase += period;
    if (phase >= period) phase -= period;
  }

  g.valid = true;
  g.periodFrames = period * hop;
  g.phaseFrames = phase * hop;
  return g;
}

// ---------------------------------------------------------------------------

void LoudnessHistory::reset(int blockFrames) {
  blockFrames_ = std::max(1, blockFrames);
  fill_ = 0;
  acc_ = 0;
  blocks_.clear();
}

void LoudnessHistory::add(const float* x, size_t frames, int channels) {
  if (blockFrames_ <= 0) return;
  for (size_t f = 0; f < frames; ++f) {
    double s = 0;
    for (int c = 0; c < channels; ++c) {
      const double v = x[f * channels + c];
      s += v * v;
    }
    acc_ += s / channels;
    if (++fill_ == blockFrames_) {
      blocks_.push_back(static_cast<float>(acc_ / blockFrames_));
      fill_ = 0;
      acc_ = 0;
    }
  }
}

int64_t detectFadeOutCut(const std::vector<float>& energy, size_t endBlock, double bps, float cutDb) {
  const size_t n = std::min(endBlock, energy.size());
  if (bps <= 0 || n < static_cast<size_t>(kFadeMinTrackSeconds * bps)) return -1;

  // Smoothed loudness in dB.
  const size_t half = std::max<size_t>(1, static_cast<size_t>(kFadeSmoothSeconds * bps / 2));
  std::vector<double> prefix(n + 1, 0.0);
  for (size_t i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + energy[i];
  std::vector<double> db(n);
  for (size_t i = 0; i < n; ++i) {
    const size_t lo = i >= half ? i - half : 0, hi = std::min(n, i + half + 1);
    db[i] = 10.0 * std::log10((prefix[hi] - prefix[lo]) / static_cast<double>(hi - lo) + 1e-12);
  }

  // Body level: a high percentile of the last 30 s.
  const size_t refStart = n > static_cast<size_t>(kFadeRefSeconds * bps) ? n - static_cast<size_t>(kFadeRefSeconds * bps) : 0;
  std::vector<double> window(db.begin() + static_cast<std::ptrdiff_t>(refStart), db.end());
  const size_t k = static_cast<size_t>(kFadeRefPercentile * (window.size() - 1));
  std::nth_element(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(k), window.end());
  const double ref = window[k];
  if (ref < kQuietTrackDb) return -1;

  // The fade starts after the last block still at full level.
  size_t start = n - 1;
  while (start > 0 && db[start] < ref - kFadeBodyDb) --start;
  if (static_cast<double>(n - 1 - start) < kFadeMinSeconds * bps) return -1;

  // It must end well below the body level...
  const size_t endLen = std::max<size_t>(1, static_cast<size_t>(0.5 * bps));
  double endLevel = 0;
  for (size_t i = n - endLen; i < n; ++i) endLevel += db[i];
  endLevel /= static_cast<double>(endLen);
  if (endLevel > ref - kFadeMinDepthDb) return -1;

  // ...and get there by falling steadily (rules out a quiet coda or a step).
  double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
  const double m = static_cast<double>(n - start);
  for (size_t i = start; i < n; ++i) {
    const double t = static_cast<double>(i - start) / bps, y = db[i];
    sx += t;
    sy += y;
    sxx += t * t;
    syy += y * y;
    sxy += t * y;
  }
  const double vx = sxx - sx * sx / m, vy = syy - sy * sy / m, cxy = sxy - sx * sy / m;
  if (vx <= 0 || vy <= 0) return -1;
  const double slope = cxy / vx;
  const double r = cxy / std::sqrt(vx * vy);
  if (slope > kFadeMinSlopeDbPerSec || r > kFadeMaxCorrelation) return -1;

  for (size_t i = start; i < n; ++i)
    if (db[i] <= ref - cutDb) return static_cast<int64_t>(i);
  return static_cast<int64_t>(n);
}

}  // namespace xf
