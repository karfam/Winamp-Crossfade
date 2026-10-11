// Engine behaviour tests. A small simulator drives the engine the way Winamp
// and a decoder do: Open, Write while CanWrite allows (rendering when the
// buffer is full), poll IsPlaying at end of input, then Close.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine.h"
#include "synth.h"
#include "test.h"

using namespace xf;

namespace {

constexpr int kRate = 44100;

struct Sim {
  Engine& e;
  int ch;
  std::vector<float> out;  // rendered output, interleaved
  int block = 441;         // 10 ms

  explicit Sim(Engine& engine) : e(engine), ch(engine.outputChannels()) {}

  void render(int frames) {
    std::vector<float> buf(static_cast<size_t>(frames) * ch);
    e.render(buf.data(), frames);
    out.insert(out.end(), buf.begin(), buf.end());
  }

  // Writes all of `pcm` (16-bit interleaved) like a decoder would.
  void writeAll(const std::vector<int16_t>& pcm, int inCh) {
    const int chunkFrames = 576;
    size_t pos = 0;
    const size_t frames = pcm.size() / inCh;
    while (pos < frames) {
      const size_t n = std::min<size_t>(chunkFrames, frames - pos);
      const int bytes = static_cast<int>(n * inCh * 2);
      if (e.canWrite() >= bytes) {
        e.write(reinterpret_cast<const char*>(&pcm[pos * inCh]), bytes);
        pos += n;
      } else {
        render(block);
      }
    }
  }

  void waitEnd() {
    while (e.isPlaying()) render(block);
  }

  void play(const std::vector<int16_t>& pcm, int rate, int inCh) {
    CHECK(e.open(rate, inCh, 16, 0, 0) >= 0);
    writeAll(pcm, inCh);
    waitEnd();
    e.close();
  }

  void drain() {
    int guard = 0;
    while (e.hasWork() && ++guard < 100000) render(block);
  }

  size_t frames() const { return out.size() / ch; }
  float at(size_t frame, int c = 0) const { return out[frame * ch + c]; }

  size_t firstNonZero() const {
    for (size_t i = 0; i < frames(); ++i)
      if (at(i) != 0.0f) return i;
    return frames();
  }
  size_t lastNonZero() const {
    for (size_t i = frames(); i-- > 0;)
      if (at(i) != 0.0f) return i;
    return 0;
  }
};

std::vector<int16_t> constant(int16_t v, size_t frames, int ch = 2) { return std::vector<int16_t>(frames * ch, v); }

Settings quietSettings() {
  Settings s;
  s.crossfade = false;
  s.trimTail = s.trimHead = false;
  s.smartFadeOut = s.beatSync = false;
  s.fadeStartMs = s.fadeStopMs = s.fadePauseMs = s.fadeSeekMs = 0;
  s.softLimiter = false;
  return s;
}

Engine* makeEngine(const Settings& s, int rate = kRate) {
  auto* e = new Engine(s);
  e->setOutputFormat(rate, 2);
  return e;
}

}  // namespace

TEST(engine_rejects_bad_formats) {
  std::unique_ptr<Engine> e(makeEngine(quietSettings()));
  CHECK(e->open(44100, 2, 12, 0, 0) < 0);
  CHECK(e->open(44100, 0, 16, 0, 0) < 0);
  CHECK(e->open(10, 2, 16, 0, 0) < 0);
  Engine unconfigured;
  CHECK(unconfigured.open(44100, 2, 16, 0, 0) < 0);
}

TEST(engine_gapless_is_sample_exact) {
  std::unique_ptr<Engine> e(makeEngine(quietSettings()));
  Sim sim(*e);
  std::vector<int16_t> a(3 * kRate * 2), b(2 * kRate * 2);
  for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<int16_t>(100 + i % 997);
  for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<int16_t>(-100 - static_cast<int>(i % 1009));
  sim.play(a, kRate, 2);
  sim.play(b, kRate, 2);
  sim.drain();

  const size_t start = sim.firstNonZero();
  std::vector<int16_t> expect = a;
  expect.insert(expect.end(), b.begin(), b.end());
  CHECK(sim.frames() - start >= expect.size() / 2);
  size_t mismatches = 0;
  for (size_t i = 0; i < expect.size() && start * 2 + i < sim.out.size(); ++i)
    if (sim.out[start * 2 + i] != expect[i] / 32768.0f) ++mismatches;
  CHECK(mismatches == 0);
  CHECK(sim.lastNonZero() - start + 1 == expect.size() / 2);
}

TEST(engine_crossfade_overlaps_exactly) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 1000;
  s.curve = Curve::EqualPower;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  const size_t lenA = 4 * kRate, lenB = 3 * kRate, xfade = kRate;
  sim.play(constant(16384, lenA), kRate, 2);  // 0.5
  sim.play(constant(8192, lenB), kRate, 2);   // 0.25
  sim.drain();

  const size_t start = sim.firstNonZero();
  size_t fadeBegin = start;
  while (fadeBegin < sim.frames() && sim.at(fadeBegin) == 0.5f) ++fadeBegin;
  size_t fadeEnd = fadeBegin;
  while (fadeEnd < sim.frames() && std::fabs(sim.at(fadeEnd) - 0.25f) > 1e-6f) ++fadeEnd;

  // The first frame of a fade is still at full gain, hence the + 1.
  CHECK(fadeBegin - start == lenA - xfade + 1);
  CHECK_NEAR(static_cast<double>(fadeEnd - fadeBegin), static_cast<double>(xfade), 2);
  CHECK(sim.lastNonZero() - start + 1 == lenA + lenB - xfade);
  // Equal power on uncorrelated material; for DC the mid point is 0.5*cos + 0.25*sin.
  const float mid = sim.at(fadeBegin + xfade / 2);
  CHECK_NEAR(mid, 0.75 * 0.70710678, 1e-3);
}

TEST(engine_crossfade_across_sample_rates) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 500;
  std::unique_ptr<Engine> e(makeEngine(s, 48000));
  Sim sim(*e);
  sim.play(constant(16384, 2 * 44100), 44100, 2);
  sim.play(constant(16384, 2 * 22050, 1), 22050, 1);
  sim.drain();
  const size_t len = sim.lastNonZero() - sim.firstNonZero() + 1;
  CHECK_NEAR(static_cast<double>(len), 2 * 48000 + 2 * 48000 - 24000, 64);
  // Same level on both sides of an equal-power fade of correlated DC peaks at
  // sqrt(2); with the soft limiter off nothing should exceed that.
  for (size_t i = 0; i < sim.frames(); ++i) CHECK(sim.at(i) <= 0.5f * 1.4143f);
}

TEST(engine_soft_limiter_prevents_clipping) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 1000;
  s.softLimiter = true;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  sim.play(constant(32000, 3 * kRate), kRate, 2);
  sim.play(constant(32000, 3 * kRate), kRate, 2);
  sim.drain();
  float peak = 0;
  for (float v : sim.out) peak = std::max(peak, std::fabs(v));
  CHECK(peak <= 1.0f);
  CHECK(peak > 0.95f);
}

TEST(engine_stop_fades_out) {
  Settings s = quietSettings();
  s.fadeStopMs = 500;
  s.curve = Curve::Linear;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  CHECK(e->open(kRate, 2, 16, 0, 0) >= 0);
  auto pcm = constant(16384, 2 * kRate);
  e->write(reinterpret_cast<const char*>(pcm.data()), static_cast<int>(pcm.size() * 2));
  sim.render(kRate / 2);
  const size_t stopAt = sim.frames();
  e->close();
  sim.drain();
  CHECK(!e->hasWork());
  const size_t fadeLen = sim.frames() - stopAt;
  CHECK_NEAR(static_cast<double>(fadeLen), kRate / 2.0, sim.block);
  // Linear fade: at the middle the level is about half.
  CHECK_NEAR(sim.at(stopAt + kRate / 4), 0.25, 0.01);
  CHECK(sim.at(stopAt + 10) > sim.at(stopAt + kRate / 3));
}

TEST(engine_manual_skip_has_no_gap) {
  Settings s = quietSettings();
  s.fadeStopMs = 300;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  CHECK(e->open(kRate, 2, 16, 0, 0) >= 0);
  auto a = constant(16384, 2 * kRate);
  e->write(reinterpret_cast<const char*>(a.data()), static_cast<int>(a.size() * 2));
  sim.render(kRate / 2);
  const size_t skipAt = sim.frames();
  e->close();  // user pressed "next"
  sim.play(constant(8192, 2 * kRate), kRate, 2);
  sim.drain();
  // From the skip until the new track is fully in, the output never drops out.
  for (size_t i = skipAt; i < skipAt + kRate / 2; ++i) CHECK(sim.at(i) > 0.1f);
  CHECK(sim.at(skipAt + kRate) == 0.25f);
}

TEST(engine_pause_fades_and_freezes_time) {
  Settings s = quietSettings();
  s.fadePauseMs = 100;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  CHECK(e->open(kRate, 2, 16, 0, 0) >= 0);
  auto pcm = constant(16384, 3 * kRate);
  e->write(reinterpret_cast<const char*>(pcm.data()), static_cast<int>(pcm.size() * 2));
  sim.render(kRate / 2);
  CHECK(e->pause(1) == 0);
  sim.render(kRate / 5);  // 200 ms: fade (100 ms) then silence
  const int t1 = e->outputTime();
  const size_t silentFrom = sim.frames();
  sim.render(kRate / 2);
  CHECK(e->outputTime() == t1);
  for (size_t i = silentFrom; i < sim.frames(); ++i) CHECK(sim.at(i) == 0.0f);
  CHECK(e->isPlaying() == 1);  // never hand off while paused
  CHECK_NEAR(t1, 600, 2);
  CHECK(e->pause(0) == 1);
  sim.render(kRate / 2);
  CHECK(sim.at(sim.frames() - 1) == 0.5f);
  CHECK(e->outputTime() > t1);
}

TEST(engine_seek_moves_time) {
  Settings s = quietSettings();
  s.fadeSeekMs = 50;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  CHECK(e->open(kRate, 2, 16, 0, 0) >= 0);
  auto pcm = constant(16384, kRate);
  e->write(reinterpret_cast<const char*>(pcm.data()), static_cast<int>(pcm.size() * 2));
  sim.render(kRate / 4);
  e->flush(60000);
  CHECK(e->writtenTime() == 60000);
  CHECK(e->outputTime() == 60000);
  auto pcm2 = constant(8192, kRate);
  e->write(reinterpret_cast<const char*>(pcm2.data()), static_cast<int>(pcm2.size() * 2));
  CHECK(e->writtenTime() == 61000);
  sim.render(kRate / 2);
  CHECK_NEAR(e->outputTime(), 60500, 2);
  CHECK(sim.at(sim.frames() - 1) == 0.25f);
}

TEST(engine_buffer_is_bounded) {
  Settings s = quietSettings();
  std::unique_ptr<Engine> e(makeEngine(s));
  CHECK(e->open(kRate, 2, 16, 0, 0) >= 0);
  std::vector<int16_t> chunk(576 * 2, 1000);
  int written = 0;
  while (e->canWrite() >= 576 * 4 && written < 10000) {
    e->write(reinterpret_cast<const char*>(chunk.data()), 576 * 4);
    ++written;
  }
  CHECK(written < 10000);
  const double bufferedMs = e->writtenTime();
  CHECK(bufferedMs <= s.bufferMs + s.handoffLeadMs + 1);
}

TEST(engine_trims_silence_between_tracks) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 1000;
  s.trimTail = s.trimHead = true;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  auto a = constant(16384, 3 * kRate);
  auto silence = constant(0, 2 * kRate);
  a.insert(a.end(), silence.begin(), silence.end());  // 3 s audio + 2 s silence
  auto b = constant(0, kRate);
  auto bAudio = constant(8192, 3 * kRate);
  b.insert(b.end(), bAudio.begin(), bAudio.end());     // 1 s silence + 3 s audio
  sim.play(a, kRate, 2);
  CHECK(e->open(kRate, 2, 16, 0, 0) >= 0);
  sim.writeAll(b, 2);
  // A still has handoff lead (1 s) + crossfade (1 s) to play; B starts at the
  // crossfade. Leading silence that was skipped counts towards B's position.
  sim.render(kRate * 2);
  CHECK_NEAR(e->outputTime(), 1000 + 1000, 20);  // 1 s skipped + 1 s played
  sim.waitEnd();
  e->close();
  sim.drain();

  const size_t start = sim.firstNonZero();
  size_t fadeBegin = start;
  while (fadeBegin < sim.frames() && sim.at(fadeBegin) == 0.5f) ++fadeBegin;
  CHECK(fadeBegin - start == 2 * kRate + 1);  // audible end (3 s) minus crossfade
  // No silent stretch anywhere between the start and the end.
  for (size_t i = start, end = sim.lastNonZero(); i <= end; ++i) CHECK(sim.at(i) != 0.0f);
  CHECK(sim.lastNonZero() - start + 1 == 3 * kRate + 3 * kRate - kRate);
}

TEST(engine_short_track_crossfade_is_limited) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 5000;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  sim.play(constant(16384, kRate / 2), kRate, 2);  // 0.5 s jingle
  sim.play(constant(8192, 3 * kRate), kRate, 2);
  sim.drain();
  const size_t start = sim.firstNonZero();
  size_t fadeBegin = start;
  while (fadeBegin < sim.frames() && sim.at(fadeBegin) == 0.5f) ++fadeBegin;
  CHECK(fadeBegin - start == kRate / 4 + 1);  // overlap is at most half the short track
}

TEST(engine_video_plays_out_before_next_track) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 1000;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  CHECK(e->open(kRate, 2, 16, -666, 0) >= 0);
  sim.writeAll(constant(16384, 2 * kRate), 2);
  sim.waitEnd();
  CHECK(!e->hasWork() || e->isPlaying() == 0);
  e->close();
  sim.play(constant(8192, kRate), kRate, 2);
  sim.drain();
  const size_t start = sim.firstNonZero();
  CHECK(sim.at(start + 2 * kRate - 1) == 0.5f);
  CHECK(sim.lastNonZero() - start + 1 >= static_cast<size_t>(3 * kRate - sim.block));
}

TEST(engine_stop_during_tail_fades_everything) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 2000;
  s.fadeStopMs = 200;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  sim.play(constant(16384, 4 * kRate), kRate, 2);  // ends naturally -> tail
  CHECK(e->open(kRate, 2, 16, 0, 0) >= 0);
  sim.writeAll(constant(8192, 4 * kRate), 2);
  sim.render(kRate);  // into the crossfade
  const size_t stopAt = sim.frames();
  e->close();         // user presses stop mid-crossfade
  sim.drain();
  CHECK(sim.frames() - stopAt <= static_cast<size_t>(kRate / 5 + sim.block));

}

TEST(engine_short_track_between_two_crossfades) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 1000;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  sim.play(constant(16384, 4 * kRate), kRate, 2);
  sim.play(constant(8192, kRate), kRate, 2);  // 1 s: overlap with C is capped at 0.5 s
  sim.play(constant(4096, 3 * kRate), kRate, 2);
  sim.drain();
  const size_t start = sim.firstNonZero();
  CHECK(sim.lastNonZero() - start + 1 == 4 * kRate + kRate + 3 * kRate - kRate - kRate / 2);
  for (size_t i = start, end = sim.lastNonZero(); i <= end; ++i) CHECK(sim.at(i) > 0.1f);
}

namespace {

std::vector<int16_t> toStereo16(const std::vector<float>& mono) {
  std::vector<int16_t> out(mono.size() * 2);
  for (size_t i = 0; i < mono.size(); ++i) {
    const float v = std::max(-1.0f, std::min(1.0f, mono[i]));
    out[2 * i] = out[2 * i + 1] = static_cast<int16_t>(std::lrint(v * 32767.0f));
  }
  return out;
}

}  // namespace

TEST(engine_beat_sync_aligns_beats) {
  for (double phaseB : {0.0, 0.13, 0.31, 0.47}) {
    Settings s = quietSettings();
    s.crossfade = true;
    s.crossfadeMs = 4000;
    s.beatSync = true;
    std::unique_ptr<Engine> e(makeEngine(s));
    Sim sim(*e);
    const double bpm = 120.0, period = 60.0 / bpm;
    const size_t lenA = 12 * kRate;
    sim.play(toStereo16(xft::drumLoop(kRate, bpm, 12.0, 0.05, 0.02f, 3)), kRate, 2);
    sim.play(toStereo16(xft::drumLoop(kRate, bpm, 10.0, phaseB, 0.02f, 4)), kRate, 2);
    sim.drain();

    const TransitionInfo info = e->lastTransition();
    CHECK(info.valid && info.beatAligned);
    CHECK_NEAR(info.bpmOut, bpm, 1.0);
    CHECK_NEAR(info.bpmIn, bpm, 1.0);
    CHECK(std::abs(info.overlapMs - 4000) <= 260);  // moved by at most half a beat
    // B starts (lenA - overlap) after A; its first kick must land on one of A's kicks.
    const double bStart = static_cast<double>(lenA - info.overlapFrames) / kRate;
    const double offset = std::fmod(bStart + phaseB - 0.05 + 10 * period, period);
    CHECK(std::min(offset, period - offset) < 0.015);
  }
}

TEST(engine_beat_sync_falls_back_without_beats) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 3000;
  s.beatSync = true;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  sim.play(toStereo16(xft::drumLoop(kRate, 120.0, 10.0, 0.0, 0.02f, 3)), kRate, 2);
  sim.play(toStereo16(xft::chord(kRate, 8.0, 0.3f)), kRate, 2);  // no beat
  sim.drain();
  const TransitionInfo info = e->lastTransition();
  CHECK(info.valid && !info.beatAligned);
  CHECK(info.overlapMs == 3000);
}

TEST(engine_smart_fade_cuts_long_fade_out) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 3000;
  s.smartFadeOut = true;
  s.smartFadeDb = 18;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  // 30 s: steady until 20 s, then a 10 s fade to -40 dB (-18 dB at 24.5 s).
  auto a = xft::drumLoop(kRate, 120.0, 30.0, 0.0, 0.05f, 9);
  xft::applyDbRamp(a, kRate, 20.0, 30.0, 0.0, -40.0);
  sim.play(toStereo16(a), kRate, 2);
  sim.play(constant(8192, 5 * kRate), kRate, 2);
  sim.drain();
  const TransitionInfo info = e->lastTransition();
  CHECK(info.valid && info.fadeOutDetected);
  CHECK_NEAR(info.fadeOutCutMs, 5500, 300);
  CHECK(info.overlapMs == 3000);
  const size_t total = sim.lastNonZero() - sim.firstNonZero() + 1;
  CHECK_NEAR(total / static_cast<double>(kRate), 24.5 + 5 - 3, 0.3);
}

TEST(engine_smart_fade_leaves_normal_endings) {
  Settings s = quietSettings();
  s.crossfade = true;
  s.crossfadeMs = 2000;
  s.smartFadeOut = true;
  std::unique_ptr<Engine> e(makeEngine(s));
  Sim sim(*e);
  sim.play(toStereo16(xft::drumLoop(kRate, 120.0, 20.0, 0.0, 0.05f, 9)), kRate, 2);
  sim.play(constant(8192, 3 * kRate), kRate, 2);
  sim.drain();
  const TransitionInfo info = e->lastTransition();
  CHECK(info.valid && !info.fadeOutDetected && info.fadeOutCutMs == 0);
}
