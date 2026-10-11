#include <cmath>
#include <cstdint>
#include <vector>

#include "analysis.h"
#include "synth.h"
#include "test.h"

using namespace xf;
using namespace xft;

namespace {

double phaseErrorMs(const BeatGrid& g, double expectPhaseSec, int rate) {
  const double expect = std::fmod(expectPhaseSec * rate, g.periodFrames);
  double d = std::fabs(g.phaseFrames - expect);
  d = std::min(d, g.periodFrames - d);  // circular distance
  return d * 1000.0 / rate;
}

}  // namespace

TEST(beats_kick_pattern) {
  const int rate = 44100;
  for (double bpm : {75.0, 100.0, 122.0, 128.0, 140.0, 174.0}) {
    auto x = drumLoop(rate, bpm, 8.0, 0.137, 0.02f, 7);
    BeatGrid g = detectBeats(x.data(), x.size(), rate);
    CHECK(g.valid);
    // Accept the tempo or its double/half (octave errors are harmless for alignment).
    const double ratio = g.bpm(rate) / bpm;
    const double octave = std::fabs(std::log2(ratio) - std::round(std::log2(ratio)));
    CHECK(octave < 0.01);
    if (bpm <= 150) CHECK(std::fabs(g.bpm(rate) - bpm) < 1.0);  // the tempo prior picks the real one
    CHECK(phaseErrorMs(g, 0.137, rate) < 15.0);
  }
}

TEST(beats_with_offbeats_keep_tempo) {
  const int rate = 48000;
  auto x = drumLoop(rate, 124.0, 10.0, 0.0, 0.02f, 11, /*offbeatSnare=*/true, /*hats=*/true);
  BeatGrid g = detectBeats(x.data(), x.size(), rate);
  CHECK(g.valid);
  CHECK(std::fabs(g.bpm(rate) - 124.0) < 1.0);
}

TEST(beats_rejects_noise_and_pads) {
  const int rate = 44100;
  auto n = noise(rate, 8.0, 0.2f, 3);
  CHECK(!detectBeats(n.data(), n.size(), rate).valid);
  auto pad = chord(rate, 8.0, 0.3f);
  CHECK(!detectBeats(pad.data(), pad.size(), rate).valid);
  std::vector<float> silence(rate * 8, 0.0f);
  CHECK(!detectBeats(silence.data(), silence.size(), rate).valid);
  auto shortLoop = drumLoop(rate, 120.0, 2.0, 0.0, 0.0f, 1);
  CHECK(!detectBeats(shortLoop.data(), shortLoop.size(), rate).valid);
}

namespace {

std::vector<float> blocksFor(const std::vector<float>& mono, int blockFrames) {
  LoudnessHistory h;
  h.reset(blockFrames);
  h.add(mono.data(), mono.size(), 1);
  return h.blocks();
}

}  // namespace

TEST(fade_out_detected_and_cut) {
  const int rate = 8000, bf = rate / 10;
  // 30 s of steady music, then a 10 s fade from 0 to -40 dB.
  auto x = drumLoop(rate, 120.0, 40.0, 0.0, 0.05f, 5);
  applyDbRamp(x, rate, 30.0, 40.0, 0.0, -40.0);
  const auto blocks = blocksFor(x, bf);
  const int64_t cut = detectFadeOutCut(blocks, blocks.size(), 10.0, 18.0f);
  CHECK(cut > 0);
  // The fade reaches -18 dB 4.5 s into the fade.
  CHECK_NEAR(cut / 10.0, 34.5, 0.8);
}

TEST(fade_out_ignores_abrupt_and_quiet_endings) {
  const int rate = 8000, bf = rate / 10;
  auto abrupt = drumLoop(rate, 120.0, 40.0, 0.0, 0.05f, 5);
  auto b1 = blocksFor(abrupt, bf);
  CHECK(detectFadeOutCut(b1, b1.size(), 10.0, 18.0f) < 0);

  // A quiet coda (a step down, not a fade).
  auto coda = drumLoop(rate, 120.0, 40.0, 0.0, 0.05f, 5);
  applyDbRamp(coda, rate, 32.0, 32.1, 0.0, -25.0);
  auto b2 = blocksFor(coda, bf);
  CHECK(detectFadeOutCut(b2, b2.size(), 10.0, 18.0f) < 0);

  // A short final decay (2 s) is a natural ending, not a fade-out.
  auto decay = drumLoop(rate, 120.0, 40.0, 0.0, 0.05f, 5);
  applyDbRamp(decay, rate, 38.0, 40.0, 0.0, -40.0);
  auto b3 = blocksFor(decay, bf);
  CHECK(detectFadeOutCut(b3, b3.size(), 10.0, 18.0f) < 0);

  std::vector<float> silent(rate * 20, 0.0f);
  auto b4 = blocksFor(silent, bf);
  CHECK(detectFadeOutCut(b4, b4.size(), 10.0, 18.0f) < 0);
}
