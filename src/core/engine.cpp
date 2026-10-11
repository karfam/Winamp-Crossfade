#include "engine.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>

#include "analysis.h"
#include "fifo.h"
#include "pcm.h"
#include "resampler.h"

namespace xf {

namespace {

constexpr int kMinFadeMs = 10;           // shorter overlaps are treated as gapless
constexpr int kMaxHeadTrimMs = 10000;    // give up skipping leading silence after this
constexpr int kLoudnessBlockMs = 100;    // loudness history resolution
constexpr int kSmartFadeLookaheadMs = 10000;  // extra read-ahead to reach fade-out cut points
constexpr int kBeatWindowMs = 12000;     // audio analysed for beats on each side
constexpr int kMinBeatWindowMs = 3000;
constexpr float kVolumeRampSeconds = 0.02f;
constexpr float kLimiterKnee = 0.9f;

float softClip(float x) {
  const float a = std::fabs(x);
  if (a <= kLimiterKnee) return x;
  const float range = 1.0f - kLimiterKnee;
  const float y = kLimiterKnee + range * std::tanh((a - kLimiterKnee) / range);
  return x < 0 ? -y : y;
}

}  // namespace

struct Engine::Track {
  // Input format as opened by Winamp.
  int inRate = 0, inCh = 0, inBps = 0, inBytesPerFrame = 0;
  bool video = false;  // bufferlenms == -666: output clock drives video sync

  // Conversion state (touched only by API calls, under the engine lock).
  std::unique_ptr<Resampler> rs;
  std::vector<float> conv, mapped, resampled;
  std::vector<char> partial;  // bytes of an incomplete frame from the last write

  AudioFifo fifo;  // converted audio in the output format

  int64_t baseMs = 0;    // track position of the first frame (set by Flush)
  int64_t inFrames = 0;  // input frames written since baseMs
  int64_t played = 0;    // output frames rendered since baseMs
  int64_t skipped = 0;   // output frames dropped as leading silence

  bool open = true;         // Winamp still owns it (between Open and Close)
  bool inputEnded = false;  // decoder finished (IsPlaying was called)
  bool drained = false;     // resampler tail flushed into the fifo
  bool armed = false;       // we told Winamp it finished: next Close is a natural end
  bool tailAnalyzed = false;  // end-of-track trimming / fade / beat analysis done
  bool headTrim = false;    // still skipping leading silence
  bool started = false;     // has begun playing
  bool buffering = false;   // underrun: wait for prebuffer before resuming
  bool dying = false;       // fading out after stop/skip/seek
  bool handedOff = false;   // tail that is crossfading into its successor

  // Analysis.
  LoudnessHistory loudness;      // per-block loudness of everything buffered
  int64_t pushed = 0;            // frames pushed into the fifo (minus tail cuts)
  bool fadeOutDetected = false;
  int64_t fadeCutFrames = 0;
  BeatGrid tailGrid;             // beats near the end of the track
  double tailBeatRem = 0;        // a beat falls when remaining() == this + k * period
  const Track* plannedFor = nullptr;  // successor the beat-sync plan was made for
  int64_t plannedOverlap = -1;   // beat-aligned overlap, -1 = none
  double plannedBpmIn = 0;

  int64_t limit = -1;       // max frames left to play, -1 = unlimited
  int64_t startFade = 0;    // fade-in length when it starts on its own
  Fade fade;

  int64_t remaining() const {
    const int64_t n = static_cast<int64_t>(fifo.size());
    return limit >= 0 ? std::min(n, limit) : n;
  }
};

Engine::Engine(const Settings& settings) : s_(settings) {
  s_.sanitize();
  pauseFade_.set(1.0f);
}

Engine::~Engine() = default;

void Engine::setSettings(const Settings& settings) {
  std::lock_guard<std::mutex> lk(mu_);
  s_ = settings;
  s_.sanitize();
  updateVolumeLocked();
}

Settings Engine::settings() const {
  std::lock_guard<std::mutex> lk(mu_);
  return s_;
}

bool Engine::setOutputFormat(int sampleRate, int channels) {
  std::lock_guard<std::mutex> lk(mu_);
  if (sampleRate <= 0 || channels <= 0 || channels > 8) return false;
  if (sampleRate == rate_ && channels == ch_) return true;
  if (!tracks_.empty()) return false;
  rate_ = sampleRate;
  ch_ = channels;
  return true;
}

bool Engine::configured() const {
  std::lock_guard<std::mutex> lk(mu_);
  return rate_ > 0;
}

int Engine::outputRate() const {
  std::lock_guard<std::mutex> lk(mu_);
  return rate_;
}

int Engine::outputChannels() const {
  std::lock_guard<std::mutex> lk(mu_);
  return ch_;
}

bool Engine::hasWork() const {
  std::lock_guard<std::mutex> lk(mu_);
  return !tracks_.empty();
}

int64_t Engine::msToFrames(int64_t ms) const { return ms * rate_ / 1000; }

int64_t Engine::capacityFrames() const {
  int64_t ms = s_.bufferMs + s_.handoffLeadMs;
  if (s_.crossfade) ms += s_.crossfadeMs + (s_.smartFadeOut ? kSmartFadeLookaheadMs : 0);
  return msToFrames(ms);
}

int64_t Engine::prebufferFrames() const { return msToFrames(s_.prebufferMs); }

std::unique_ptr<Engine::Track> Engine::makeTrack(int rate, int ch, int bps) const {
  auto t = std::make_unique<Track>();
  t->inRate = rate;
  t->inCh = ch;
  t->inBps = bps;
  t->inBytesPerFrame = ch * bps / 8;
  t->rs = std::make_unique<Resampler>(rate, rate_, ch_);
  t->fifo.reset(ch_, static_cast<size_t>(capacityFrames() + msToFrames(500)));
  t->fade.set(0.0f);
  t->loudness.reset(static_cast<int>(msToFrames(kLoudnessBlockMs)));
  return t;
}

// ---------------------------------------------------------------------------
// Winamp API

int Engine::open(int sampleRate, int channels, int bps, int bufferLenMs, int /*prebufferMs*/) {
  std::lock_guard<std::mutex> lk(mu_);
  if (rate_ <= 0) return -1;
  if (sampleRate < 1000 || sampleRate > 1536000 || channels < 1 || channels > 8 || !isSupportedBps(bps))
    return -1;
  if (current_) closeLocked();
  if (paused_) {
    paused_ = false;
    pauseFade_.set(1.0f);
  }

  auto t = makeTrack(sampleRate, channels, bps);
  t->video = bufferLenMs == -666;

  bool manualChange = false;
  for (auto& tr : tracks_) manualChange |= tr->dying;
  const bool autoChange = tailBeforeLocked(tracks_.size()) != nullptr;
  t->startFade = msToFrames(manualChange ? s_.fadeStopMs : s_.fadeStartMs);
  t->headTrim = autoChange && s_.crossfade && s_.trimHead && !t->video;

  current_ = t.get();
  tracks_.push_back(std::move(t));
  return static_cast<int>(capacityFrames() * 1000 / rate_);
}

void Engine::close() {
  std::lock_guard<std::mutex> lk(mu_);
  closeLocked();
}

void Engine::closeLocked() {
  Track* t = current_;
  if (!t) return;
  current_ = nullptr;
  t->open = false;

  if (paused_) {
    // Nothing is audible; drop everything rather than resume it later.
    tracks_.clear();
    return;
  }
  if (t->armed) return;  // natural end: keep playing as a tail

  // Stop or manual skip: fade out everything that is still sounding.
  const int64_t fade = msToFrames(s_.fadeStopMs);
  for (auto& tr : tracks_) makeDyingLocked(tr.get(), fade);
  reapLocked();
}

void Engine::makeDyingLocked(Track* t, int64_t fadeFrames) {
  t->open = false;
  if (!t->started || fadeFrames <= 0) {
    t->dying = true;
    t->limit = 0;
    return;
  }
  const int64_t n = std::min(fadeFrames, t->remaining());
  if (t->dying && t->limit >= 0 && t->limit <= n) return;  // already fading out faster
  t->dying = true;
  t->limit = n;
  t->fade.start(t->fade.gain(), 0.0f, n, s_.curve);
}

int Engine::write(const char* data, int len) {
  std::lock_guard<std::mutex> lk(mu_);
  Track* t = current_;
  if (!t || !data || len <= 0) return 0;

  if (t->inputEnded) {
    // The decoder was not done after all.
    t->inputEnded = t->armed = t->drained = t->tailAnalyzed = false;
    t->tailGrid = BeatGrid();
  }

  const char* src = data;
  size_t bytes = static_cast<size_t>(len);
  std::vector<char> joined;
  if (!t->partial.empty()) {
    joined.assign(t->partial.begin(), t->partial.end());
    joined.insert(joined.end(), data, data + len);
    t->partial.clear();
    src = joined.data();
    bytes = joined.size();
  }
  const size_t bpf = static_cast<size_t>(t->inBytesPerFrame);
  const size_t frames = bytes / bpf;
  if (bytes % bpf) t->partial.assign(src + frames * bpf, src + bytes);
  if (frames == 0) return 0;

  t->conv.resize(frames * t->inCh);
  pcmToFloat(src, frames, t->inCh, t->inBps, t->conv.data());
  const float* mixIn = t->conv.data();
  if (t->inCh != ch_) {
    t->mapped.resize(frames * ch_);
    mapChannels(t->conv.data(), t->inCh, t->mapped.data(), ch_, frames);
    mixIn = t->mapped.data();
  }
  t->inFrames += static_cast<int64_t>(frames);

  if (t->rs->passthrough()) {
    pushLocked(t, mixIn, frames);
  } else {
    t->resampled.clear();
    t->rs->process(mixIn, frames, t->resampled);
    pushLocked(t, t->resampled.data(), t->resampled.size() / ch_);
  }
  return 0;
}

void Engine::pushLocked(Track* t, const float* data, size_t frames) {
  if (t->headTrim && frames > 0) {
    const float thr = dbToGain(static_cast<float>(s_.silenceThresholdDb));
    size_t first = frames;
    for (size_t i = 0; i < frames * ch_; ++i) {
      if (std::fabs(data[i]) > thr) {
        first = i / ch_;
        break;
      }
    }
    t->skipped += static_cast<int64_t>(first);
    if (first < frames || t->skipped >= msToFrames(kMaxHeadTrimMs)) t->headTrim = false;
    data += first * ch_;
    frames -= first;
  }
  t->loudness.add(data, frames, ch_);
  t->pushed += static_cast<int64_t>(frames);
  t->fifo.push(data, frames);
}

int Engine::canWrite() {
  std::lock_guard<std::mutex> lk(mu_);
  Track* t = current_;
  if (!t) return 0;
  const int64_t freeOut = capacityFrames() - static_cast<int64_t>(t->fifo.size());
  if (freeOut <= 0) return 0;
  int64_t freeIn = freeOut * t->inRate / rate_ - t->rs->latencyFrames();
  if (freeIn <= 0) return 0;
  const int64_t bytes = freeIn * t->inBytesPerFrame - static_cast<int64_t>(t->partial.size());
  return static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(bytes, INT_MAX / 2)));
}

int Engine::isPlaying() {
  std::lock_guard<std::mutex> lk(mu_);
  Track* t = current_;
  if (!t) return 0;

  if (!t->inputEnded) {
    t->inputEnded = true;
    t->buffering = false;
  }
  if (!t->drained) {
    t->drained = true;
    t->partial.clear();
    if (!t->rs->passthrough()) {
      t->resampled.clear();
      t->rs->drain(t->resampled);
      pushLocked(t, t->resampled.data(), t->resampled.size() / ch_);
    }
  }
  if (t->armed) return 0;

  // Video: the output clock drives sync, so play everything out normally.
  if (t->video) return t->fifo.empty() ? 0 : 1;
  // Don't let Winamp move on while paused; it would unpause on the next Open.
  if (paused_) return 1;

  if (s_.crossfade && !t->tailAnalyzed) {
    if (s_.trimTail) trimTailLocked(t);
    if (s_.smartFadeOut) smartFadeLocked(t);
    if (s_.beatSync) analyzeTailBeatsLocked(t);
    t->tailAnalyzed = true;
  }
  const int64_t handoff = msToFrames(s_.handoffLeadMs + (s_.crossfade ? s_.crossfadeMs : 0));
  if (t->remaining() <= handoff) {
    t->armed = true;
    return 0;
  }
  return 1;
}

void Engine::trimTailLocked(Track* t) {
  const float thr = dbToGain(static_cast<float>(s_.silenceThresholdDb));
  size_t keep = t->fifo.size();
  while (keep > 0) {
    const float* f = t->fifo.frame(keep - 1);
    bool loud = false;
    for (int c = 0; c < ch_; ++c) loud |= std::fabs(f[c]) > thr;
    if (loud) break;
    --keep;
  }
  truncateTailLocked(t, keep);
}

void Engine::truncateTailLocked(Track* t, size_t keep) {
  const size_t size = t->fifo.size();
  if (keep >= size) return;
  t->fifo.truncate(keep);
  t->pushed -= static_cast<int64_t>(size - keep);
}

void Engine::smartFadeLocked(Track* t) {
  const int bf = t->loudness.blockFrames();
  if (bf <= 0 || t->pushed <= 0) return;
  const int64_t cut = detectFadeOutCut(t->loudness.blocks(), static_cast<size_t>(t->pushed / bf),
                                       static_cast<double>(rate_) / bf, static_cast<float>(s_.smartFadeDb));
  if (cut < 0) return;
  t->fadeOutDetected = true;
  const int64_t size = static_cast<int64_t>(t->fifo.size());
  const int64_t front = t->pushed - size;
  // Audio already played can't be cut; keep at least a full crossfade.
  const int64_t keep = std::max(cut * bf - front, std::min(size, msToFrames(s_.crossfadeMs)));
  if (keep >= size) return;
  t->fadeCutFrames = size - keep;
  truncateTailLocked(t, static_cast<size_t>(keep));
}

std::vector<float> Engine::monoLocked(const Track* t, size_t from, size_t frames) const {
  std::vector<float> mono(frames);
  const float scale = 1.0f / static_cast<float>(ch_);
  for (size_t i = 0; i < frames; ++i) {
    const float* f = t->fifo.frame(from + i);
    float s = 0;
    for (int c = 0; c < ch_; ++c) s += f[c];
    mono[i] = s * scale;
  }
  return mono;
}

void Engine::analyzeTailBeatsLocked(Track* t) {
  const size_t size = t->fifo.size();
  const size_t len = std::min(size, static_cast<size_t>(msToFrames(kBeatWindowMs)));
  if (static_cast<int64_t>(len) < msToFrames(kMinBeatWindowMs)) return;
  const std::vector<float> mono = monoLocked(t, size - len, len);
  const BeatGrid g = detectBeats(mono.data(), len, rate_);
  if (!g.valid) return;
  t->tailGrid = g;
  // remaining() when the first detected beat plays, folded into one period.
  t->tailBeatRem = std::fmod(static_cast<double>(len) - g.phaseFrames, g.periodFrames);
}

// Picks the overlap length that lands the incoming track's beats on the
// outgoing track's beats, as close as possible to the nominal length.
void Engine::planBeatSyncLocked(Track* tail, Track* next, int64_t nominal, int64_t rem) {
  tail->plannedFor = next;
  tail->plannedOverlap = -1;
  tail->plannedBpmIn = 0;
  const size_t len = std::min(next->fifo.size(), static_cast<size_t>(msToFrames(kBeatWindowMs)));
  if (static_cast<int64_t>(len) < msToFrames(kMinBeatWindowMs)) return;
  const std::vector<float> mono = monoLocked(next, 0, len);
  const BeatGrid g = detectBeats(mono.data(), len, rate_);
  if (!g.valid) return;
  tail->plannedBpmIn = g.bpm(rate_);

  // Starting the next track when remaining() == r puts its first beat
  // (g.phaseFrames in) on an outgoing beat when r = tailBeatRem + phase + k * period.
  const double period = tail->tailGrid.periodFrames;
  double best = -1, bestErr = 0;
  for (double r = tail->tailBeatRem + g.phaseFrames; r <= static_cast<double>(rem); r += period) {
    if (r < static_cast<double>(msToFrames(kMinFadeMs))) continue;
    const double err = std::fabs(r - static_cast<double>(nominal));
    if (best < 0 || err < bestErr) {
      best = r;
      bestErr = err;
    }
  }
  if (best > 0 && bestErr <= period) tail->plannedOverlap = std::llround(best);
}

int Engine::pause(int pause) {
  std::lock_guard<std::mutex> lk(mu_);
  const int prev = paused_ ? 1 : 0;
  const bool want = pause != 0;
  if (want == paused_) return prev;
  paused_ = want;
  pauseFade_.start(pauseFade_.gain(), want ? 0.0f : 1.0f, msToFrames(s_.fadePauseMs), s_.curve);
  return prev;
}

void Engine::setVolume(int volume) {
  std::lock_guard<std::mutex> lk(mu_);
  if (volume >= 0) lastVolume_ = std::min(volume, 255);  // -666 means "re-apply"
  updateVolumeLocked();
}

void Engine::setPan(int pan) {
  std::lock_guard<std::mutex> lk(mu_);
  lastPan_ = std::max(-128, std::min(pan, 128));
  updateVolumeLocked();
}

void Engine::updateVolumeLocked() {
  const bool use = s_.useWinampVolume;
  // Squared slider position: a rough perceptual taper without a hard floor.
  const float v = lastVolume_ / 255.0f;
  volTarget_ = use ? v * v : 1.0f;
  const int pan = use ? lastPan_ : 0;
  panL_ = pan > 0 ? 1.0f - pan / 128.0f : 1.0f;
  panR_ = pan < 0 ? 1.0f + pan / 128.0f : 1.0f;
}

void Engine::flush(int timeMs) {
  std::lock_guard<std::mutex> lk(mu_);
  Track* old = current_;
  if (!old) return;

  auto t = makeTrack(old->inRate, old->inCh, old->inBps);
  t->video = old->video;
  t->baseMs = std::max(0, timeMs);
  t->startFade = msToFrames(s_.fadeSeekMs);

  if (paused_) {
    old->open = false;
    old->dying = true;
    old->limit = 0;
  } else {
    makeDyingLocked(old, msToFrames(s_.fadeSeekMs));
  }
  current_ = t.get();
  tracks_.push_back(std::move(t));
  reapLocked();
}

int Engine::outputTime() {
  std::lock_guard<std::mutex> lk(mu_);
  const Track* t = current_;
  if (!t || rate_ <= 0) return 0;
  double ms = static_cast<double>(t->baseMs);
  if (t->started) {
    ms += static_cast<double>(t->played + t->skipped) * 1000.0 / rate_ - latencyMs_.load();
    ms = std::max(ms, static_cast<double>(t->baseMs));
  }
  return static_cast<int>(ms);
}

int Engine::writtenTime() {
  std::lock_guard<std::mutex> lk(mu_);
  const Track* t = current_;
  if (!t) return 0;
  return static_cast<int>(t->baseMs + t->inFrames * 1000 / t->inRate);
}

// ---------------------------------------------------------------------------
// Mixing

bool Engine::readyLocked(const Track* t) const {
  return t->inputEnded || !t->open || static_cast<int64_t>(t->fifo.size()) >= prebufferFrames();
}

// The track that `tracks_[index]` would continue from after a natural end.
// (Tracks start in order, so for a pending track this tail has started.)
Engine::Track* Engine::tailBeforeLocked(size_t index) const {
  for (size_t i = index; i-- > 0;) {
    Track* t = tracks_[i].get();
    if (t->dying) continue;
    if (t->armed && !t->open && !t->handedOff) return t;
    return nullptr;
  }
  return nullptr;
}

Engine::Track* Engine::firstUnstartedLocked(size_t* index) const {
  for (size_t i = 0; i < tracks_.size(); ++i) {
    Track* t = tracks_[i].get();
    if (!t->started && !(t->dying && t->limit == 0)) {
      if (index) *index = i;
      return t;
    }
  }
  return nullptr;
}

bool Engine::awaitingHandoffLocked(const Track* t) const {
  if (!t->armed || t->handedOff || t->dying) return false;
  size_t idx = 0;
  const Track* next = firstUnstartedLocked(&idx);
  return next && readyLocked(next) && tailBeforeLocked(idx) == t;
}

void Engine::startPendingLocked(int64_t& segment) {
  for (;;) {
    size_t idx = 0;
    Track* p = firstUnstartedLocked(&idx);
    if (!p || !readyLocked(p)) return;

    Track* tail = tailBeforeLocked(idx);
    if (tail) {
      const int64_t tailLength = msToFrames(tail->baseMs) + tail->played + tail->remaining();
      const int64_t nominal =
          s_.crossfade && !p->video ? std::min(msToFrames(s_.crossfadeMs), tailLength / 2) : 0;
      const int64_t rem = tail->remaining();
      int64_t overlap = nominal;
      const bool beatSync = nominal > 0 && s_.beatSync && tail->tailGrid.valid;
      if (beatSync) {
        if (tail->plannedFor != p) {
          // Decide one beat before the nominal start, so an earlier beat is still reachable.
          const int64_t decideAt = nominal + static_cast<int64_t>(std::ceil(tail->tailGrid.periodFrames));
          if (rem > decideAt) {
            segment = std::min(segment, rem - decideAt);
            return;
          }
          planBeatSyncLocked(tail, p, nominal, rem);
        }
        if (tail->plannedOverlap > 0) overlap = tail->plannedOverlap;
      }
      if (rem > overlap) {
        // Keep playing the tail alone until the overlap point.
        segment = std::min(segment, rem - overlap);
        return;
      }
      tail->handedOff = true;
      if (rem >= msToFrames(kMinFadeMs)) {
        tail->limit = rem;
        tail->fade.start(tail->fade.gain(), 0.0f, rem, s_.curve);
        p->fade.start(0.0f, 1.0f, rem, s_.curve);

        TransitionInfo info;
        info.valid = true;
        info.overlapFrames = rem;
        info.overlapMs = static_cast<int>(rem * 1000 / rate_);
        info.nominalMs = static_cast<int>(nominal * 1000 / rate_);
        info.beatSyncTried = s_.beatSync;
        info.beatAligned = beatSync && tail->plannedFor == p && tail->plannedOverlap == rem;
        info.bpmOut = tail->tailGrid.bpm(rate_);
        info.bpmIn = tail->plannedFor == p ? tail->plannedBpmIn : 0;
        info.fadeOutDetected = tail->fadeOutDetected;
        info.fadeOutCutMs = static_cast<int>(tail->fadeCutFrames * 1000 / rate_);
        lastTransition_ = info;
      } else {
        p->fade.set(1.0f);  // gapless
      }
    } else if (p->startFade > 0) {
      p->fade.start(0.0f, 1.0f, p->startFade, s_.curve);
    } else {
      p->fade.set(1.0f);
    }
    p->started = true;
  }
}

void Engine::mixTrackLocked(Track* t, float* out, int64_t frames) {
  const int64_t n = std::min(frames, t->remaining());
  for (int64_t i = 0; i < n; ++i) {
    const float g = t->fade.next();
    const float* f = t->fifo.frame(static_cast<size_t>(i));
    float* o = out + i * ch_;
    for (int c = 0; c < ch_; ++c) o[c] += f[c] * g;
  }
  if (n > 0) {
    t->fifo.discard(static_cast<size_t>(n));
    t->played += n;
    if (t->limit >= 0) t->limit -= n;
  }
  if (n < frames && t->open && !t->inputEnded) t->buffering = true;
}

void Engine::mixTracksLocked(float* out, int64_t frames) {
  int64_t pos = 0;
  while (pos < frames) {
    int64_t seg = frames - pos;
    startPendingLocked(seg);
    for (auto& tr : tracks_) {
      Track* t = tr.get();
      if (!t->started) continue;
      if (t->buffering) {
        if (!readyLocked(t)) continue;
        t->buffering = false;
      }
      mixTrackLocked(t, out + pos * ch_, seg);
    }
    pos += seg;
    reapLocked();
  }
}

void Engine::reapLocked() {
  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                               [this](const TrackPtr& tr) {
                                 const Track* t = tr.get();
                                 if (t->open) return false;
                                 if (t->dying && t->limit == 0) return true;
                                 return t->remaining() == 0 && !awaitingHandoffLocked(t);
                               }),
                tracks_.end());
}

void Engine::applyMasterLocked(float* out, int64_t frames) {
  const float volStep = 1.0f / (kVolumeRampSeconds * rate_);
  for (int64_t i = 0; i < frames; ++i) {
    const float pg = pauseFade_.next();
    if (volCurrent_ < volTarget_) volCurrent_ = std::min(volTarget_, volCurrent_ + volStep);
    else if (volCurrent_ > volTarget_) volCurrent_ = std::max(volTarget_, volCurrent_ - volStep);
    const float g = pg * volCurrent_;
    float* o = out + i * ch_;
    for (int c = 0; c < ch_; ++c) {
      float s = o[c] * g;
      if (c == 0) s *= panL_;
      else if (c == 1) s *= panR_;
      if (s_.softLimiter) s = softClip(s);
      o[c] = s;
    }
  }
}

TransitionInfo Engine::lastTransition() const {
  std::lock_guard<std::mutex> lk(mu_);
  return lastTransition_;
}

std::string describeTransition(const TransitionInfo& info) {
  if (!info.valid) return "No automatic crossfade yet.";
  char buf[256];
  std::string s;
  std::snprintf(buf, sizeof buf, "%.2f s crossfade", info.overlapMs / 1000.0);
  s = buf;
  if (info.beatAligned) {
    // Without changing tempo, beats only stay together if the tempos match
    // (allowing for double/half time); otherwise just the first beat lines up.
    double ratio = info.bpmIn > 0 ? info.bpmOut / info.bpmIn : 1.0;
    while (ratio > 1.5) ratio /= 2;
    while (ratio < 0.75) ratio *= 2;
    const bool together = std::fabs(ratio - 1.0) < 0.04;
    std::snprintf(buf, sizeof buf, "; %s (%.1f -> %.1f BPM, %+.2f s)",
                  together ? "beats aligned" : "first beat aligned", info.bpmOut, info.bpmIn,
                  (info.overlapMs - info.nominalMs) / 1000.0);
    s += buf;
  } else if (info.beatSyncTried) {
    s += "; no clear beat to align";
  }
  if (info.fadeOutDetected) {
    std::snprintf(buf, sizeof buf, "; fade-out detected (skipped last %.1f s)", info.fadeOutCutMs / 1000.0);
    s += buf;
  }
  return s;
}

void Engine::render(float* out, int frames) {
  std::lock_guard<std::mutex> lk(mu_);
  if (frames <= 0) return;
  std::fill(out, out + static_cast<size_t>(frames) * std::max(ch_, 1), 0.0f);
  if (rate_ <= 0) return;

  int64_t done = 0;
  while (done < frames) {
    if (paused_ && !pauseFade_.active()) break;  // fully paused: silence, consume nothing
    int64_t chunk = frames - done;
    if (paused_) chunk = std::min(chunk, pauseFade_.remaining());
    mixTracksLocked(out + done * ch_, chunk);
    applyMasterLocked(out + done * ch_, chunk);
    done += chunk;
  }
}

}  // namespace xf
