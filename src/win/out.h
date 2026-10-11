#pragma once

// Winamp output plugin interface (binary compatible with the Winamp SDK's
// out.h, OUT_VER 0x10). Winamp loads out_*.dll from its Plugins folder and
// calls winampGetOutModule() to get this table.

#include <windows.h>

#include <cstdint>

#define OUT_VER 0x10

struct Out_Module {
  int version;          // OUT_VER
  char* description;    // shown in Preferences
  intptr_t id;          // non-Nullsoft plugins use >= 65536

  HWND hMainWindow;     // filled in by Winamp
  HINSTANCE hDllInstance;  // filled in by Winamp

  void (*Config)(HWND hwndParent);
  void (*About)(HWND hwndParent);

  void (*Init)();
  void (*Quit)();

  // Returns >= 0 (max latency in ms) on success, < 0 on failure.
  // bufferlenms == -666 means the output clock is used to sync video.
  int (*Open)(int samplerate, int numchannels, int bitspersamp, int bufferlenms, int prebufferms);
  void (*Close)();
  // 0 on success. Never blocks.
  int (*Write)(char* buf, int len);
  // Bytes that can be written right now.
  int (*CanWrite)();
  // Non-zero while output is still playing. Called by the decoder after its
  // last Write(); returning 0 lets Winamp move on to the next track.
  int (*IsPlaying)();
  int (*Pause)(int pause);  // returns previous pause state

  void (*SetVolume)(int volume);  // 0..255 (-666: re-apply current)
  void (*SetPan)(int pan);        // -128..128

  void (*Flush)(int t);  // seek: drop buffered audio, restart at t ms

  int (*GetOutputTime)();   // ms actually heard
  int (*GetWrittenTime)();  // ms written
};
