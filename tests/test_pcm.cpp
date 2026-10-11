#include <cstdint>
#include <vector>

#include "pcm.h"
#include "test.h"

using namespace xf;

TEST(pcm_8bit_unsigned) {
  const uint8_t in[] = {0, 128, 255};
  float out[3];
  CHECK(pcmToFloat(in, 3, 1, 8, out));
  CHECK_NEAR(out[0], -1.0, 1e-6);
  CHECK_NEAR(out[1], 0.0, 1e-6);
  CHECK_NEAR(out[2], 127.0 / 128.0, 1e-6);
}

TEST(pcm_16bit) {
  const int16_t in[] = {-32768, 0, 16384, 32767};
  float out[4];
  CHECK(pcmToFloat(in, 2, 2, 16, out));
  CHECK_NEAR(out[0], -1.0, 1e-6);
  CHECK_NEAR(out[1], 0.0, 1e-6);
  CHECK_NEAR(out[2], 0.5, 1e-6);
  CHECK_NEAR(out[3], 32767.0 / 32768.0, 1e-6);
}

TEST(pcm_24bit_packed) {
  // 0x400000 (= 0.5), 0xC00000 (= -0.5), 0x800000 (= -1.0)
  const uint8_t in[] = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x80};
  float out[3];
  CHECK(pcmToFloat(in, 3, 1, 24, out));
  CHECK_NEAR(out[0], 0.5, 1e-7);
  CHECK_NEAR(out[1], -0.5, 1e-7);
  CHECK_NEAR(out[2], -1.0, 1e-7);
}

TEST(pcm_32bit) {
  const int32_t in[] = {INT32_MIN, 1 << 30};
  float out[2];
  CHECK(pcmToFloat(in, 2, 1, 32, out));
  CHECK_NEAR(out[0], -1.0, 1e-7);
  CHECK_NEAR(out[1], 0.5, 1e-7);
}

TEST(pcm_rejects_unknown_bps) {
  float out[1];
  const uint8_t in[4] = {};
  CHECK(!pcmToFloat(in, 1, 1, 12, out));
  CHECK(!isSupportedBps(12));
  CHECK(isSupportedBps(24));
}

TEST(map_mono_to_stereo_and_back) {
  const float mono[] = {0.25f, -0.5f};
  float st[4];
  mapChannels(mono, 1, st, 2, 2);
  CHECK(st[0] == 0.25f && st[1] == 0.25f && st[2] == -0.5f && st[3] == -0.5f);
  const float stereo[] = {1.0f, 0.0f};
  float m[1];
  mapChannels(stereo, 2, m, 1, 1);
  CHECK_NEAR(m[0], 0.5, 1e-7);
}

TEST(map_stereo_to_surround_device) {
  const float st[] = {0.1f, 0.2f};
  float out[6];
  mapChannels(st, 2, out, 6, 1);
  CHECK(out[0] == 0.1f && out[1] == 0.2f);
  for (int c = 2; c < 6; ++c) CHECK(out[c] == 0.0f);
}

TEST(map_51_downmix_does_not_clip) {
  const float in[] = {1, 1, 1, 1, 1, 1};
  float out[2];
  mapChannels(in, 6, out, 2, 1);
  CHECK(out[0] <= 1.0001f && out[1] <= 1.0001f);
  CHECK(out[0] > 0.9f && out[1] > 0.9f);
}
