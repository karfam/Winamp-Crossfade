// Offline crossfade simulator: plays WAV files through the same Engine the
// Winamp plugin uses, driving it exactly the way Winamp and its decoders do,
// and writes the result to a WAV file. Useful for hearing settings without
// Winamp, and on any OS.
//
//   xfade_sim [options] out.wav in1.wav in2.wav [...]
//     --crossfade MS   crossfade length, 0 = gapless (default 5000)
//     --curve NAME     linear | power | scurve | log (default power)
//     --rate HZ        mix rate (default: first input's rate)
//     --no-trim        do not skip silence between tracks
//     --threshold DB   silence threshold, e.g. -55
//     --skip-at SEC    simulate pressing "next" SEC seconds into each track
//     --no-beat-sync   do not align beats
//     --no-smart-fade  do not detect fade-outs

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine.h"
#include "wav.h"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: xfade_sim [--crossfade MS] [--curve linear|power|scurve|log] [--rate HZ]\n"
               "                 [--no-trim] [--threshold DB] [--skip-at SEC] [--no-beat-sync]\n"
               "                 [--no-smart-fade] out.wav in1.wav in2.wav ...\n");
}

struct Renderer {
  xf::Engine& engine;
  int channels;
  std::vector<float> out;
  std::vector<float> block;
  int blockFrames;

  Renderer(xf::Engine& e, int rate) : engine(e), channels(e.outputChannels()), blockFrames(rate / 100) {
    block.resize(static_cast<size_t>(blockFrames) * channels);
  }
  void tick() {
    engine.render(block.data(), blockFrames);
    out.insert(out.end(), block.begin(), block.end());
    const xf::TransitionInfo info = engine.lastTransition();
    if (info.valid && !reported(info)) {
      last = info;
      std::printf("  at %7.2f s: %s\n", static_cast<double>(out.size()) / channels / (blockFrames * 100),
                  xf::describeTransition(info).c_str());
    }
  }
  bool reported(const xf::TransitionInfo& info) const {
    return last.valid && info.overlapFrames == last.overlapFrames && info.bpmIn == last.bpmIn &&
           info.bpmOut == last.bpmOut && info.fadeOutCutMs == last.fadeOutCutMs;
  }
  xf::TransitionInfo last;
};

}  // namespace

int main(int argc, char** argv) {
  xf::Settings s;
  int rate = 0;
  double skipAt = -1;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--crossfade") {
      s.crossfadeMs = std::atoi(next());
      s.crossfade = s.crossfadeMs > 0;
    } else if (a == "--curve") {
      const std::string c = next();
      if (c == "linear") s.curve = xf::Curve::Linear;
      else if (c == "power") s.curve = xf::Curve::EqualPower;
      else if (c == "scurve") s.curve = xf::Curve::SCurve;
      else if (c == "log") s.curve = xf::Curve::Logarithmic;
      else {
        usage();
        return 2;
      }
    } else if (a == "--rate") {
      rate = std::atoi(next());
    } else if (a == "--no-trim") {
      s.trimHead = s.trimTail = false;
    } else if (a == "--threshold") {
      s.silenceThresholdDb = std::atoi(next());
    } else if (a == "--skip-at") {
      skipAt = std::atof(next());
    } else if (a == "--no-beat-sync") {
      s.beatSync = false;
    } else if (a == "--no-smart-fade") {
      s.smartFadeOut = false;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      files.push_back(a);
    }
  }
  if (files.size() < 2) {
    usage();
    return 2;
  }
  s.sanitize();

  std::vector<WavData> inputs(files.size() - 1);
  for (size_t i = 1; i < files.size(); ++i) {
    std::string err;
    if (!readWav(files[i], inputs[i - 1], err)) {
      std::fprintf(stderr, "error: %s\n", err.c_str());
      return 1;
    }
  }
  if (rate <= 0) rate = inputs[0].rate;

  xf::Engine engine(s);
  engine.setOutputFormat(rate, 2);
  Renderer r(engine, rate);

  for (size_t t = 0; t < inputs.size(); ++t) {
    const WavData& w = inputs[t];
    if (engine.open(w.rate, w.channels, w.bps, 0, 0) < 0) {
      std::fprintf(stderr, "error: engine rejected %s\n", files[t + 1].c_str());
      return 1;
    }
    const size_t frameBytes = static_cast<size_t>(w.channels) * w.bps / 8;
    const size_t chunk = 576 * frameBytes;
    const size_t skipBytes =
        skipAt >= 0 ? static_cast<size_t>(skipAt * w.rate) * frameBytes : w.pcm.size();
    bool skipped = false;
    size_t pos = 0;
    while (pos < w.pcm.size()) {
      if (pos >= skipBytes && engine.outputTime() >= skipAt * 1000) {
        skipped = true;  // user pressed "next"
        break;
      }
      const size_t n = std::min(chunk, w.pcm.size() - pos);
      if (engine.canWrite() >= static_cast<int>(n)) {
        engine.write(reinterpret_cast<const char*>(&w.pcm[pos]), static_cast<int>(n));
        pos += n;
      } else {
        r.tick();
      }
    }
    if (!skipped) {
      while (engine.isPlaying()) r.tick();
    }
    engine.close();
  }
  while (engine.hasWork()) r.tick();

  if (!writeWav16(files[0], r.out, rate, r.channels)) {
    std::fprintf(stderr, "error: cannot write %s\n", files[0].c_str());
    return 1;
  }
  std::printf("wrote %s: %.2f s at %d Hz\n", files[0].c_str(),
              static_cast<double>(r.out.size()) / r.channels / rate, rate);
  return 0;
}
