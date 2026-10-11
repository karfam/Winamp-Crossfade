#include "fade.h"
#include "test.h"

using namespace xf;

TEST(curve_endpoints) {
  for (int c = 0; c < kCurveCount; ++c) {
    CHECK_NEAR(curveIn(static_cast<Curve>(c), 0.0f), 0.0, 1e-6);
    CHECK_NEAR(curveIn(static_cast<Curve>(c), 1.0f), 1.0, 1e-6);
    // monotonic
    float prev = -1.0f;
    for (int i = 0; i <= 100; ++i) {
      const float v = curveIn(static_cast<Curve>(c), i / 100.0f);
      CHECK(v >= prev);
      prev = v;
    }
  }
}

TEST(equal_power_crossfade_keeps_power) {
  Fade in, out;
  in.start(0.0f, 1.0f, 1000, Curve::EqualPower);
  out.start(1.0f, 0.0f, 1000, Curve::EqualPower);
  for (int i = 0; i < 1000; ++i) {
    const float a = in.next(), b = out.next();
    CHECK_NEAR(a * a + b * b, 1.0, 1e-4);
  }
  CHECK(!in.active() && !out.active());
  CHECK(in.gain() == 1.0f && out.gain() == 0.0f);
}

TEST(linear_and_scurve_crossfade_keep_gain) {
  for (Curve c : {Curve::Linear, Curve::SCurve}) {
    Fade in, out;
    in.start(0.0f, 1.0f, 500, c);
    out.start(1.0f, 0.0f, 500, c);
    for (int i = 0; i < 500; ++i) CHECK_NEAR(in.next() + out.next(), 1.0, 1e-5);
  }
}

TEST(fade_out_from_partial_gain) {
  Fade f;
  f.start(0.5f, 0.0f, 100, Curve::Linear);
  CHECK_NEAR(f.next(), 0.5, 1e-6);
  for (int i = 1; i < 100; ++i) f.next();
  CHECK(!f.active());
  CHECK(f.gain() == 0.0f);
}

TEST(zero_length_fade_jumps) {
  Fade f;
  f.set(1.0f);
  f.start(1.0f, 0.0f, 0, Curve::EqualPower);
  CHECK(!f.active());
  CHECK(f.next() == 0.0f);
}
