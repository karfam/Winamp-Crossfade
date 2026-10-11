#pragma once

#include <map>
#include <string>

#include "fade.h"

namespace xf {

struct Settings {
  // Crossfade on automatic track change.
  bool crossfade = true;
  int crossfadeMs = 5000;
  Curve curve = Curve::EqualPower;
  // Crossfade from the last audible sample of a track into the first audible
  // sample of the next, skipping silence (only when crossfading).
  bool trimTail = true;
  bool trimHead = true;
  int silenceThresholdDb = -55;
  // Treat the point where a gradual fade-out has dropped smartFadeDb below the
  // song's level as its end, so the next track arrives during the fade.
  bool smartFadeOut = true;
  int smartFadeDb = 18;
  // Shift the crossfade by up to half a beat so both tracks' beats line up.
  bool beatSync = true;

  // Fades for user actions.
  int fadeStartMs = 0;    // fade-in when playback starts from stopped
  int fadeStopMs = 800;   // fade-out on stop, crossfade on manual track change
  int fadePauseMs = 250;  // fade on pause / resume
  int fadeSeekMs = 60;    // crossfade between old and new position on seek

  // Buffering. Total buffer = bufferMs + handoffLeadMs + crossfadeMs
  // (+ 10 s read-ahead with smartFadeOut, to reach cut points of long fades).
  int bufferMs = 2000;
  int prebufferMs = 250;      // audio needed before a track starts playing
  int handoffLeadMs = 1000;   // how early Winamp is told to open the next track

  // Output device.
  std::string deviceId;       // UTF-8 endpoint id; empty = system default
  int deviceBufferMs = 40;
  bool softLimiter = true;
  bool useWinampVolume = true;

  void sanitize();

  std::map<std::string, std::string> toKeyValues() const;
  // Unknown keys are ignored; missing or malformed keys keep their current value.
  void fromKeyValues(const std::map<std::string, std::string>& kv);

  bool operator==(const Settings& o) const;
  bool operator!=(const Settings& o) const { return !(*this == o); }
};

}  // namespace xf
