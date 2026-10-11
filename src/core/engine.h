#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "fade.h"
#include "settings.h"

namespace xf {

// The crossfading mixer behind the Winamp output plugin. It is independent of
// Windows: the plugin forwards the Out_Module calls to it, and an output
// backend pulls mixed audio through render(). All methods are thread-safe.
//
// How a crossfade happens with Winamp's output API:
//  * Each Open()..Close() is a "track" with its own buffer, converted to the
//    mix format (output rate and channel count).
//  * When the decoder has written everything it calls IsPlaying() in a loop.
//    We report "finished" while still holding crossfade + lead time of audio,
//    so Winamp closes the track and opens the next one early.
//  * The closed track keeps playing as a "tail". Once the next track has
//    prebuffered and the tail has exactly crossfade-length left, both are
//    faded against each other, sample-accurately. With crossfading off the
//    next track starts on the frame after the tail ends (gapless).
//  * Close() without that end-of-track handshake is a user stop or skip:
//    everything fades out quickly, and a track opened during that fade fades in.
class Engine {
 public:
  explicit Engine(const Settings& settings = Settings());
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void setSettings(const Settings& settings);
  Settings settings() const;

  // Mix format. Can only change while no tracks exist; returns false otherwise.
  bool setOutputFormat(int sampleRate, int channels);
  bool configured() const;
  int outputRate() const;
  int outputChannels() const;

  // True while anything is playing, fading or waiting to play.
  bool hasWork() const;
  // Audio already handed to the device but not yet heard; used for timing.
  void setOutputLatencyMs(double ms) { latencyMs_.store(ms); }

  // ---- Winamp Out_Module semantics ----
  int open(int sampleRate, int channels, int bps, int bufferLenMs, int prebufferMs);
  void close();
  int write(const char* data, int len);
  int canWrite();
  int isPlaying();
  int pause(int pause);
  void setVolume(int volume);  // 0..255
  void setPan(int pan);        // -128..128
  void flush(int timeMs);
  int outputTime();
  int writtenTime();

  // Mixes `frames` interleaved frames in the output format into `out`.
  void render(float* out, int frames);

 private:
  struct Track;
  using TrackPtr = std::unique_ptr<Track>;

  int64_t msToFrames(int64_t ms) const;
  int64_t capacityFrames() const;
  int64_t prebufferFrames() const;
  std::unique_ptr<Track> makeTrack(int rate, int ch, int bps) const;

  void closeLocked();
  void makeDyingLocked(Track* t, int64_t fadeFrames);
  void pushLocked(Track* t, const float* data, size_t frames);
  void trimTailLocked(Track* t);
  bool readyLocked(const Track* t) const;
  Track* tailBeforeLocked(size_t index) const;
  Track* firstUnstartedLocked(size_t* index) const;
  bool awaitingHandoffLocked(const Track* t) const;
  void startPendingLocked(int64_t& segment);
  void mixTracksLocked(float* out, int64_t frames);
  void mixTrackLocked(Track* t, float* out, int64_t frames);
  void applyMasterLocked(float* out, int64_t frames);
  void updateVolumeLocked();
  void reapLocked();

  mutable std::mutex mu_;
  Settings s_;
  int rate_ = 0;
  int ch_ = 0;
  std::vector<TrackPtr> tracks_;  // in play order
  Track* current_ = nullptr;      // the track Winamp is writing to

  bool paused_ = false;
  Fade pauseFade_;
  float volTarget_ = 1.0f;
  float volCurrent_ = 1.0f;
  float panL_ = 1.0f, panR_ = 1.0f;
  int lastVolume_ = 255;
  int lastPan_ = 0;

  std::atomic<double> latencyMs_{0.0};
};

}  // namespace xf
