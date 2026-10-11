#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <cerrno>

namespace xf {

namespace {

int clampi(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }

bool parseInt(const std::string& s, int& out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str() || *end != '\0') return false;
  out = static_cast<int>(std::max<long>(std::min<long>(v, 1000000000L), -1000000000L));
  return true;
}

}  // namespace

void Settings::sanitize() {
  crossfadeMs = clampi(crossfadeMs, 0, 20000);
  curve = static_cast<Curve>(clampi(static_cast<int>(curve), 0, kCurveCount - 1));
  silenceThresholdDb = clampi(silenceThresholdDb, -96, -20);
  fadeStartMs = clampi(fadeStartMs, 0, 10000);
  fadeStopMs = clampi(fadeStopMs, 0, 10000);
  fadePauseMs = clampi(fadePauseMs, 0, 5000);
  fadeSeekMs = clampi(fadeSeekMs, 0, 2000);
  bufferMs = clampi(bufferMs, 500, 20000);
  prebufferMs = clampi(prebufferMs, 0, bufferMs / 2);
  handoffLeadMs = clampi(handoffLeadMs, 200, 5000);
  deviceBufferMs = clampi(deviceBufferMs, 10, 500);
}

std::map<std::string, std::string> Settings::toKeyValues() const {
  auto b = [](bool v) { return std::string(v ? "1" : "0"); };
  auto i = [](int v) { return std::to_string(v); };
  return {
      {"crossfade", b(crossfade)},
      {"crossfade_ms", i(crossfadeMs)},
      {"curve", i(static_cast<int>(curve))},
      {"trim_tail", b(trimTail)},
      {"trim_head", b(trimHead)},
      {"silence_db", i(silenceThresholdDb)},
      {"fade_start_ms", i(fadeStartMs)},
      {"fade_stop_ms", i(fadeStopMs)},
      {"fade_pause_ms", i(fadePauseMs)},
      {"fade_seek_ms", i(fadeSeekMs)},
      {"buffer_ms", i(bufferMs)},
      {"prebuffer_ms", i(prebufferMs)},
      {"handoff_lead_ms", i(handoffLeadMs)},
      {"device_id", deviceId},
      {"device_buffer_ms", i(deviceBufferMs)},
      {"soft_limiter", b(softLimiter)},
      {"use_winamp_volume", b(useWinampVolume)},
  };
}

void Settings::fromKeyValues(const std::map<std::string, std::string>& kv) {
  auto getInt = [&](const char* key, int& dst) {
    auto it = kv.find(key);
    int v;
    if (it != kv.end() && parseInt(it->second, v)) dst = v;
  };
  auto getBool = [&](const char* key, bool& dst) {
    int v = dst ? 1 : 0;
    getInt(key, v);
    dst = v != 0;
  };
  getBool("crossfade", crossfade);
  getInt("crossfade_ms", crossfadeMs);
  int c = static_cast<int>(curve);
  getInt("curve", c);
  curve = static_cast<Curve>(c);
  getBool("trim_tail", trimTail);
  getBool("trim_head", trimHead);
  getInt("silence_db", silenceThresholdDb);
  getInt("fade_start_ms", fadeStartMs);
  getInt("fade_stop_ms", fadeStopMs);
  getInt("fade_pause_ms", fadePauseMs);
  getInt("fade_seek_ms", fadeSeekMs);
  getInt("buffer_ms", bufferMs);
  getInt("prebuffer_ms", prebufferMs);
  getInt("handoff_lead_ms", handoffLeadMs);
  if (auto it = kv.find("device_id"); it != kv.end()) deviceId = it->second;
  getInt("device_buffer_ms", deviceBufferMs);
  getBool("soft_limiter", softLimiter);
  getBool("use_winamp_volume", useWinampVolume);
  sanitize();
}

bool Settings::operator==(const Settings& o) const { return toKeyValues() == o.toKeyValues(); }

}  // namespace xf
