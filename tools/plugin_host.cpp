// Smoke test for the built plugin DLL: loads out_crossfade.dll and drives it
// through its Out_Module table the way Winamp does (two generated tracks,
// decoder-style writes, IsPlaying handshake, Close, Quit), printing timing.
//
//   plugin_host [path\to\out_crossfade.dll] [seconds per track]

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/win/out.h"

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "out_crossfade.dll";
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 8;

  HMODULE dll = LoadLibraryA(path);
  if (!dll) {
    std::printf("FAIL: cannot load %s (error %lu)\n", path, GetLastError());
    return 1;
  }
  using GetModule = Out_Module* (*)();
  auto get = reinterpret_cast<GetModule>(reinterpret_cast<void*>(GetProcAddress(dll, "winampGetOutModule")));
  Out_Module* m = get ? get() : nullptr;
  if (!m || m->version != OUT_VER) {
    std::printf("FAIL: winampGetOutModule missing or wrong version\n");
    return 1;
  }
  std::printf("loaded: %s\n", m->description);
  m->hMainWindow = nullptr;
  m->hDllInstance = dll;
  m->Init();
  m->SetVolume(255);
  m->SetPan(0);

  const int rate = 44100, ch = 2;
  const DWORD t0 = GetTickCount();
  const double freqs[] = {440.0, 660.0};
  for (int track = 0; track < 2; ++track) {
    const int latency = m->Open(rate, ch, 16, 0, 0);
    std::printf("[%5.2fs] Open track %d -> %d ms\n", (GetTickCount() - t0) / 1000.0, track + 1, latency);
    if (latency < 0) return 1;
    const int total = rate * seconds;
    std::vector<short> chunk(576 * ch);
    int frame = 0, lastReport = -1;
    while (frame < total) {
      const int bytes = static_cast<int>(chunk.size() * sizeof(short));
      if (m->CanWrite() >= bytes) {
        for (int i = 0; i < 576; ++i, ++frame)
          for (int c = 0; c < ch; ++c)
            chunk[i * ch + c] = static_cast<short>(8000 * std::sin(2 * 3.14159265 * freqs[track] * frame / rate));
        m->Write(reinterpret_cast<char*>(chunk.data()), bytes);
      } else {
        Sleep(10);
      }
      const int out = m->GetOutputTime() / 1000;
      if (out != lastReport) {
        lastReport = out;
        std::printf("[%5.2fs]   track %d output %d ms, written %d ms\n", (GetTickCount() - t0) / 1000.0, track + 1,
                    m->GetOutputTime(), m->GetWrittenTime());
      }
    }
    while (m->IsPlaying()) Sleep(10);
    std::printf("[%5.2fs] track %d finished (IsPlaying == 0), output %d ms\n", (GetTickCount() - t0) / 1000.0,
                track + 1, m->GetOutputTime());
    m->Close();
  }
  m->Quit();
  std::printf("[%5.2fs] Quit\n", (GetTickCount() - t0) / 1000.0);
  FreeLibrary(dll);
  std::printf("OK\n");
  return 0;
}
