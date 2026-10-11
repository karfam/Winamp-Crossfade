# Winamp Crossfade Output

A free, open-source crossfading output plugin for Winamp: a clean replacement
for *SqrSoft Advanced Crossfading Output*, without the adware.

* **Crossfades between tracks**, sample-accurate and always the full length
  you set. The next track is requested early and buffered before the fade
  starts, so a slow decoder doesn't shorten the fade.
* **Gapless playback** when crossfading is off: the next track starts on the
  exact sample after the previous one ends.
* **Silence skipping**: fades go from the last *audible* sample of a track
  into the first audible sample of the next, so trailing and leading silence
  doesn't create dead air.
* **Fade-out detection**: when a song ends in a long, gradual fade-out, the
  next song comes in *during* the fade instead of after it has died away.
* **Beat sync**: the plugin detects the tempo and beats on both sides of the
  transition. It shifts the crossfade by up to half a beat so the incoming
  track's beats land on the outgoing track's beats. Tracks without a clear
  beat get a normal crossfade.
* **Smooth fades** on stop, manual skip (a short crossfade), pause/resume
  and seek. There are no clicks.
* **Four fade curves**: equal power (default), linear, S-curve, logarithmic.
* **Mixes tracks of different formats**: any sample rate, 8/16/24/32-bit,
  mono to 7.1. A high-quality windowed-sinc resampler converts everything to
  the device's mix format.
* **Modern output**: WASAPI shared mode with MMCSS priority. The plugin
  follows the Windows default device, recovers from unplugged devices, and
  releases the device when idle.
* **Soft limiter** stops clipping when two loud tracks overlap.
* No ads, no tracking, no network access. It's a single self-contained DLL
  with no runtime to install.

## Install

1. Download `out_crossfade.dll` from the latest
   [release](../../releases/latest) (or build it, see below).
2. Close Winamp and copy the DLL into Winamp's `Plugins` folder (e.g.
   `C:\Program Files (x86)\Winamp\Plugins`).
3. Start Winamp, open **Preferences → Plug-ins → Output**, select
   **Crossfade Output** and click **Configure** to adjust settings.

Requires Windows 7 or later and Winamp 5.x (32-bit). WACUP, which loads
Winamp output plugins, should also work.

## Settings

| Setting | Default | What it does |
|---|---|---|
| Crossfade between tracks | on | Overlap the end of a track with the start of the next. When off, playback is gapless. |
| Crossfade length | 5000 ms | Length of the overlap. Never more than half the length of the track that's ending. |
| Fade curve | Equal power | Equal power keeps loudness steady for unrelated songs. Linear/S-curve suit similar material. |
| Skip silence at end / start | on | Crossfade from the last to the first audible sample. Only used when crossfading. |
| Silence threshold | -55 dB | Level below which audio counts as silence. |
| Detect fade-outs | on | If a track ends with a gradual fade-out, treat the point where it has faded 18 dB as the end. Adds 10 s of read-ahead. The depth can be changed with `smart_fade_db` in the INI file. |
| Beat sync | on | Move the crossfade start by up to half a beat so the beats line up. |
| Fade in when playback starts | 0 ms | Fade-in after pressing Play from stopped. |
| Fade out on stop / manual skip | 800 ms | Stop fades out; pressing Next crossfades over this length. |
| Fade on pause and resume | 250 ms | |
| Crossfade on seek | 60 ms | Short crossfade between the old and new position. |
| Device | System default | Any active output device. The default follows Windows' default device. |
| Buffer | 2000 ms | Extra read-ahead on top of the crossfade length. |
| Device latency | 40 ms | WASAPI buffer size. Raise it if you hear dropouts. |
| Soft limiter | on | Gently limits peaks above -1 dB instead of clipping. |
| Use Winamp's volume and balance | on | Turn off to always output at full scale. |

Settings are stored in `out_crossfade.ini` in Winamp's settings folder
(`%APPDATA%\Winamp\Plugins` for a standard install).

## How it works

Winamp's output-plugin API has no concept of "the next track", so a
crossfader has to infer it:

1. When a decoder has written its last data it polls `IsPlaying()`. The
   plugin answers "finished" while it still holds *crossfade + 1 s* of audio.
   Winamp then closes the track and opens the next one early.
2. The finished track keeps playing as a *tail*. The new track buffers. When
   the tail has exactly the crossfade length left, both are faded against
   each other on the same sample.
3. A `Close()` *without* that handshake means the user pressed Stop or Next.
   Everything fades out quickly, and a track opened during that fade fades
   in.

**Fade-out detection.** The plugin keeps a loudness history of each track
(one value per 100 ms). At the end of a track it looks for a gradual fade
with these properties:

* at least 3 s long
* ends at least 15 dB below the song's level
* falls steadily rather than in one step

If it finds one, it cuts the track where the fade reaches -18 dB, and the
crossfade ends there. Quiet codas, abrupt endings and short final decays are
left alone.

**Beat sync.** An onset detector covers the low, mid and high bands, so
kicks, snares and hats all count. Autocorrelation finds the tempo
(60–200 BPM) and a comb filter finds where the beats fall. This runs on the
last 12 s of the outgoing track and the first 12 s of the incoming one. The
plugin then picks the crossfade start, within half a beat of the configured
length, that puts the new track's first beat on a beat of the old one.

The plugin does not change tempo. Beats stay locked through the fade when the
tempos match, including double and half time. When they don't, only the first
beat lines up, so true tempo matching would need time-stretching. Audio
without a clear beat (ambient, classical, spoken word) is detected and gets
an ordinary crossfade.

The settings dialog shows what the last crossfade did, for example
`4.88 s crossfade; beats aligned (124.3 -> 125.7 BPM, -0.12 s); fade-out
detected (skipped last 6.6 s)`.

All of this lives in a portable C++17 engine (`src/core`), which is unit
tested on any OS. The Windows-specific parts (`src/win`) are the Winamp glue,
the WASAPI backend and the settings dialog.

One side effect: Winamp shows the next track (at 0:00) for about a second
before you hear it, and at the end of a playlist Winamp shows "stopped"
while the last few seconds are still playing. Every crossfading output
plugin has these quirks.

## Building

The plugin must be **32-bit**, like Winamp.

**Windows (Visual Studio 2019+ with CMake):**

```bat
cmake -S . -B build -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release
```

The plugin is `build\Release\out_crossfade.dll`.

**Linux, cross-compiling with MinGW-w64:**

```sh
sudo apt install g++-mingw-w64-i686
cmake -S . -B build-win32 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake
cmake --build build-win32
```

**Engine tests on any OS:** `cmake -S . -B build && cmake --build build && ctest --test-dir build`

### Making a release

Bump `VERSION` in `CMakeLists.txt`, commit, then tag and push:

```sh
git tag v0.3.0
git push origin v0.3.0
```

The `release` workflow builds and tests the plugin with Visual Studio. It
then publishes a GitHub Release with `out_crossfade.dll` and a zip (DLL,
README, license and `xfade_sim`), plus install steps and SHA-256 checksums.
It refuses to publish if the tag doesn't match the version in
`CMakeLists.txt`. Every other push still builds and tests, with the DLL
available as an Actions artifact.

### Tools

* `xfade_sim` renders WAV files through the same engine, driven exactly the
  way Winamp drives the plugin, so you can try settings without Winamp:

  ```sh
  xfade_sim --crossfade 6000 --curve power mix.wav track1.wav track2.wav track3.wav
  xfade_sim --skip-at 30 mix.wav a.wav b.wav    # simulate pressing Next at 0:30
  ```

  The simulator prints what each transition did (beats, fade-outs). Use
  `--no-beat-sync` and `--no-smart-fade` to compare.

* `plugin_host.exe` (Windows only) loads the built DLL and plays two
  generated tones through it via the Winamp plugin interface, as a smoke
  test.

## Status and roadmap

Version 0.2.0. Version 0.1.0 was tested in Winamp. The engine, beat
detection and fade-out detection are covered by unit tests with synthetic
music. They have not yet been tuned on a large library of real songs, so
please report transitions that sound wrong. The "Last crossfade" line in the
settings dialog makes that easy.

Possible next steps:

* Album-aware mode: play consecutive tracks from the same album gaplessly
  and crossfade only between albums
* Separate fade lengths for Stop and for Next
* Tempo matching with time-stretching (true DJ-style beatmatching)
* Loudness matching across the crossfade
* WASAPI exclusive mode and ASIO output

## License

[MIT](LICENSE): free to use, modify and redistribute, including in other
projects, as long as the copyright notice is kept.
