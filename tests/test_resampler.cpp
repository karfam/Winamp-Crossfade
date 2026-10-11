#include <algorithm>
#include <cmath>
#include <vector>

#include "resampler.h"
#include "test.h"

using namespace xf;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(int rate, double hz, size_t frames, int ch) {
  std::vector<float> v(frames * ch);
  for (size_t i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c) v[i * ch + c] = static_cast<float>(0.5 * std::sin(2 * kPi * hz * i / rate));
  return v;
}

// Feeds `in` in irregular chunks, like a decoder would.
std::vector<float> resampleChunked(Resampler& rs, const std::vector<float>& in, int ch) {
  std::vector<float> out;
  const size_t frames = in.size() / ch;
  size_t pos = 0, chunk = 1;
  while (pos < frames) {
    const size_t n = std::min(chunk, frames - pos);
    rs.process(in.data() + pos * ch, n, out);
    pos += n;
    chunk = chunk * 3 % 1153 + 1;
  }
  rs.drain(out);
  return out;
}

}  // namespace

TEST(resampler_passthrough_is_exact) {
  Resampler rs(44100, 44100, 2);
  CHECK(rs.passthrough());
  auto in = sine(44100, 440, 1000, 2);
  auto out = resampleChunked(rs, in, 2);
  CHECK(out == in);
}

TEST(resampler_output_length) {
  const int pairs[][2] = {{44100, 48000}, {48000, 44100}, {96000, 48000}, {22050, 48000}, {8000, 44100}};
  for (auto& p : pairs) {
    Resampler rs(p[0], p[1], 2);
    const size_t inFrames = 12345;
    auto out = resampleChunked(rs, std::vector<float>(inFrames * 2, 0.1f), 2);
    const double expected = std::ceil(inFrames * static_cast<double>(p[1]) / p[0]);
    CHECK_NEAR(out.size() / 2.0, expected, 1.0);
  }
}

TEST(resampler_preserves_sine) {
  const int pairs[][2] = {{44100, 48000}, {48000, 44100}, {96000, 48000}};
  for (auto& p : pairs) {
    Resampler rs(p[0], p[1], 1);
    const double hz = 1000.0;
    auto out = resampleChunked(rs, sine(p[0], hz, p[0], 1), 1);
    // Compare against the ideal sine at the output rate, away from the edges.
    double errSq = 0, refSq = 0;
    for (size_t i = 200; i + 200 < out.size(); ++i) {
      const double ref = 0.5 * std::sin(2 * kPi * hz * i / p[1]);
      errSq += (out[i] - ref) * (out[i] - ref);
      refSq += ref * ref;
    }
    const double snrDb = 10 * std::log10(refSq / errSq);
    CHECK(snrDb > 70.0);
  }
}

TEST(resampler_dc_gain_is_unity) {
  Resampler rs(44100, 48000, 1);
  auto out = resampleChunked(rs, std::vector<float>(20000, 0.3f), 1);
  for (size_t i = 100; i + 100 < out.size(); ++i) CHECK_NEAR(out[i], 0.3, 1e-4);
}

TEST(resampler_downsampling_rejects_aliases) {
  // 30 kHz at 96 kHz -> 48 kHz cannot be represented and must be filtered out.
  Resampler rs(96000, 48000, 1);
  auto out = resampleChunked(rs, sine(96000, 30000, 96000, 1), 1);
  double sq = 0;
  size_t n = 0;
  for (size_t i = 500; i + 500 < out.size(); ++i, ++n) sq += out[i] * out[i];
  const double rmsDb = 10 * std::log10(sq / n + 1e-30);
  CHECK(rmsDb < -60.0);
}
