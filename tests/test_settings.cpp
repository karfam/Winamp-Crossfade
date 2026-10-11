#include "settings.h"
#include "test.h"

using namespace xf;

TEST(settings_roundtrip) {
  Settings a;
  a.crossfade = false;
  a.crossfadeMs = 7300;
  a.curve = Curve::SCurve;
  a.trimHead = false;
  a.silenceThresholdDb = -48;
  a.fadeStopMs = 1200;
  a.deviceId = "{0.0.0.00000000}.{abc}";
  a.softLimiter = false;
  Settings b;
  b.fromKeyValues(a.toKeyValues());
  CHECK(a == b);
}

TEST(settings_sanitize_clamps) {
  Settings s;
  std::map<std::string, std::string> kv = {
      {"crossfade_ms", "999999"}, {"curve", "42"}, {"silence_db", "10"},
      {"buffer_ms", "1"},         {"prebuffer_ms", "999999"}};
  s.fromKeyValues(kv);
  CHECK(s.crossfadeMs == 20000);
  CHECK(static_cast<int>(s.curve) == kCurveCount - 1);
  CHECK(s.silenceThresholdDb == -20);
  CHECK(s.bufferMs == 500);
  CHECK(s.prebufferMs == 250);
}

TEST(settings_ignore_garbage) {
  Settings s;
  const Settings defaults;
  s.fromKeyValues({{"crossfade_ms", "abc"}, {"fade_stop_ms", ""}, {"unknown", "1"}});
  CHECK(s == defaults);
}
