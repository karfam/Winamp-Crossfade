#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xf {

// ---------------------------------------------------------------------------
// Beat detection

struct BeatGrid {
  bool valid = false;        // false when the audio has no clear, steady beat
  double periodFrames = 0;   // frames per beat
  double phaseFrames = 0;    // first beat at or after the start, 0 <= phase < period
  float confidence = 0;      // periodicity strength, roughly 0..1

  double bpm(int rate) const { return valid && periodFrames > 0 ? 60.0 * rate / periodFrames : 0.0; }
};

// Estimates tempo (60..200 BPM) and beat phase of `frames` mono samples using
// a multi-band onset envelope, autocorrelation and comb filtering. Needs at
// least 3 seconds of audio.
BeatGrid detectBeats(const float* mono, size_t frames, int rate);

// ---------------------------------------------------------------------------
// Fade-out detection

// Loudness history of a track: mean square per fixed-size block, recorded as
// audio is buffered so it covers the whole track, not just what is buffered.
class LoudnessHistory {
 public:
  void reset(int blockFrames);
  void add(const float* interleaved, size_t frames, int channels);
  int blockFrames() const { return blockFrames_; }
  const std::vector<float>& blocks() const { return blocks_; }

 private:
  int blockFrames_ = 0;
  int fill_ = 0;
  double acc_ = 0;
  std::vector<float> blocks_;
};

// If the audio ending at block `endBlock` finishes with a gradual fade-out,
// returns the block where the fade has dropped `cutDb` below the song's body
// level; that point can serve as the effective end of the track. Returns -1
// when there is no fade-out (abrupt ending, quiet section, final chord...).
int64_t detectFadeOutCut(const std::vector<float>& blockEnergy, size_t endBlock, double blocksPerSecond,
                         float cutDb);

}  // namespace xf
