#pragma once

#include <cstddef>
#include <vector>

namespace xf {

// Streaming band-limited resampler (Kaiser-windowed sinc, polyphase table with
// linear interpolation between phases). Equal rates are passed through
// bit-exactly. Output is aligned with the input: the first output frame
// corresponds to the first input frame, and drain() emits the tail.
class Resampler {
 public:
  Resampler(int inRate, int outRate, int channels);

  bool passthrough() const { return in_ == out_; }
  int inRate() const { return in_; }
  int outRate() const { return out_; }

  // Appends resampled interleaved frames to `out`.
  void process(const float* in, size_t frames, std::vector<float>& out);
  // Flushes the input still held for look-ahead and resets the stream.
  void drain(std::vector<float>& out);
  void reset();

  // Input frames held back for look-ahead (worst case).
  int latencyFrames() const { return passthrough() ? 0 : half_; }

 private:
  void build();
  void run(std::vector<float>& out);

  static constexpr int kPhases = 256;

  int in_, out_, ch_;
  int half_ = 0;
  double step_ = 1.0;
  double pos_ = 0.0;
  std::vector<float> table_;  // (kPhases + 1) rows of 2 * half_ taps
  std::vector<float> buf_;    // pending input, interleaved
  std::vector<float> coef_;
};

}  // namespace xf
