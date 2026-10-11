#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace xf {

// Growable ring buffer of interleaved float frames. Not thread-safe.
class AudioFifo {
 public:
  void reset(int channels, size_t reserveFrames) {
    ch_ = channels;
    cap_ = std::max<size_t>(reserveFrames, 1);
    buf_.assign(cap_ * static_cast<size_t>(ch_), 0.0f);
    head_ = size_ = 0;
  }

  int channels() const { return ch_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  void push(const float* src, size_t frames) {
    if (frames == 0) return;
    if (size_ + frames > cap_) grow(size_ + frames);
    const size_t tail = (head_ + size_) % cap_;
    const size_t first = std::min(frames, cap_ - tail);
    std::memcpy(&buf_[tail * ch_], src, first * ch_ * sizeof(float));
    if (frames > first)
      std::memcpy(&buf_[0], src + first * ch_, (frames - first) * ch_ * sizeof(float));
    size_ += frames;
  }

  // Frame i counted from the oldest buffered frame.
  const float* frame(size_t i) const {
    size_t idx = head_ + i;
    if (idx >= cap_) idx -= cap_;
    return &buf_[idx * ch_];
  }

  // Drops frames from the front (oldest).
  void discard(size_t frames) {
    frames = std::min(frames, size_);
    head_ = (head_ + frames) % cap_;
    size_ -= frames;
    if (size_ == 0) head_ = 0;
  }

  // Drops frames from the back (newest), keeping the first newSize frames.
  void truncate(size_t newSize) {
    if (newSize < size_) size_ = newSize;
    if (size_ == 0) head_ = 0;
  }

  void clear() { head_ = size_ = 0; }

 private:
  void grow(size_t need) {
    const size_t ncap = std::max(need, cap_ * 2);
    std::vector<float> nb(ncap * ch_);
    for (size_t i = 0; i < size_; ++i) std::memcpy(&nb[i * ch_], frame(i), ch_ * sizeof(float));
    buf_.swap(nb);
    cap_ = ncap;
    head_ = 0;
  }

  int ch_ = 1;
  size_t cap_ = 0;
  size_t head_ = 0;
  size_t size_ = 0;
  std::vector<float> buf_;
};

}  // namespace xf
