#include "wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void put32(std::FILE* f, uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                        static_cast<uint8_t>(v >> 24)};
  std::fwrite(b, 1, 4, f);
}
void put16(std::FILE* f, uint16_t v) {
  const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
  std::fwrite(b, 1, 2, f);
}

}  // namespace

bool readWav(const std::string& path, WavData& out, std::string& error) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  std::vector<uint8_t> file;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) file.insert(file.end(), buf, buf + n);
  std::fclose(f);

  if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) || std::memcmp(file.data() + 8, "WAVE", 4)) {
    error = path + " is not a RIFF/WAVE file";
    return false;
  }
  int format = 0;
  bool haveFmt = false;
  const uint8_t* data = nullptr;
  size_t dataSize = 0;
  for (size_t pos = 12; pos + 8 <= file.size();) {
    const uint8_t* chunk = file.data() + pos;
    const size_t size = le32(chunk + 4);
    const size_t avail = std::min(size, file.size() - pos - 8);
    if (!std::memcmp(chunk, "fmt ", 4) && avail >= 16) {
      format = le16(chunk + 8);
      out.channels = le16(chunk + 10);
      out.rate = static_cast<int>(le32(chunk + 12));
      out.bps = le16(chunk + 22);
      if (format == 0xFFFE && avail >= 40) format = le16(chunk + 32);  // WAVE_FORMAT_EXTENSIBLE subformat
      haveFmt = true;
    } else if (!std::memcmp(chunk, "data", 4)) {
      data = chunk + 8;
      dataSize = avail;
    }
    pos += 8 + size + (size & 1);
  }
  if (!haveFmt || !data || out.channels <= 0 || out.rate <= 0) {
    error = path + ": missing fmt or data chunk";
    return false;
  }
  if (format == 1 && (out.bps == 8 || out.bps == 16 || out.bps == 24 || out.bps == 32)) {
    out.pcm.assign(data, data + dataSize);
    return true;
  }
  if (format == 3 && out.bps == 32) {
    const size_t count = dataSize / 4;
    out.pcm.resize(count * 4);
    for (size_t i = 0; i < count; ++i) {
      float v;
      std::memcpy(&v, data + i * 4, 4);
      v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
      const int32_t s = static_cast<int32_t>(std::lrint(static_cast<double>(v) * 2147483647.0));
      std::memcpy(&out.pcm[i * 4], &s, 4);
    }
    return true;
  }
  error = path + ": unsupported sample format (need 8/16/24/32-bit PCM or 32-bit float)";
  return false;
}

bool writeWav16(const std::string& path, const std::vector<float>& samples, int rate, int channels) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2);
  std::fwrite("RIFF", 1, 4, f);
  put32(f, 36 + dataBytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, static_cast<uint16_t>(channels));
  put32(f, static_cast<uint32_t>(rate));
  put32(f, static_cast<uint32_t>(rate * channels * 2));
  put16(f, static_cast<uint16_t>(channels * 2));
  put16(f, 16);
  std::fwrite("data", 1, 4, f);
  put32(f, dataBytes);
  for (float x : samples) {
    x = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
    put16(f, static_cast<uint16_t>(static_cast<int16_t>(std::lrint(x * 32767.0f))));
  }
  return std::fclose(f) == 0;
}
