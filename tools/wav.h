#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal WAV I/O for the simulator.
struct WavData {
  int rate = 0;
  int channels = 0;
  int bps = 0;                 // bits per sample of `pcm` (8/16/24/32 integer)
  std::vector<uint8_t> pcm;    // interleaved little-endian integer PCM

  size_t frames() const { return channels && bps ? pcm.size() / (static_cast<size_t>(channels) * bps / 8) : 0; }
};

// Reads integer PCM (8/16/24/32-bit) and 32-bit float WAV files (float is
// converted to 32-bit integer). Returns false and fills `error` on failure.
bool readWav(const std::string& path, WavData& out, std::string& error);

// Writes 16-bit PCM from interleaved float samples.
bool writeWav16(const std::string& path, const std::vector<float>& samples, int rate, int channels);
