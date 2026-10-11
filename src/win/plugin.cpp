// Winamp output plugin entry point: forwards the Out_Module calls to the
// crossfade Engine and drives a WasapiOutput for playback.

#include <memory>
#include <string>

#include "engine.h"
#include "out.h"
#include "settings.h"
#include "wasapi_output.h"
#include "win_support.h"

namespace {

constexpr intptr_t kModuleId = 0x58464144;  // arbitrary, >= 65536
constexpr UINT kWmWaIpc = WM_USER;
constexpr LPARAM kIpcGetIniDirectory = 335;
constexpr LPARAM kIpcGetIniDirectoryW = 1335;
constexpr int kOpenTimeoutMs = 2000;
constexpr int kQuitDrainMs = 1500;

HINSTANCE g_instance = nullptr;
xf::Settings g_settings;
bool g_settingsLoaded = false;
std::unique_ptr<xf::Engine> g_engine;
std::unique_ptr<xf::WasapiOutput> g_output;

char g_description[] = "Crossfade Output v" XF_VERSION " (WASAPI)";

extern Out_Module g_module;

std::wstring iniPath() {
  std::wstring dir;
  HWND wa = g_module.hMainWindow;
  if (wa && IsWindow(wa)) {
    const LRESULT w = SendMessageW(wa, kWmWaIpc, 0, kIpcGetIniDirectoryW);
    if (static_cast<ULONG_PTR>(w) > 0xFFFF) {
      dir = reinterpret_cast<const wchar_t*>(w);
    } else {
      const LRESULT a = SendMessageW(wa, kWmWaIpc, 0, kIpcGetIniDirectory);
      if (static_cast<ULONG_PTR>(a) > 0xFFFF) {
        const char* s = reinterpret_cast<const char*>(a);
        const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        if (n > 1) {
          dir.resize(static_cast<size_t>(n - 1));
          MultiByteToWideChar(CP_ACP, 0, s, -1, &dir[0], n);
        }
      }
    }
  }
  if (dir.empty()) {
    // Fall back to the folder this DLL lives in.
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_instance, path, MAX_PATH);
    dir = path;
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? L"." : dir.substr(0, slash);
  }
  // Winamp 5 keeps plugin settings in a Plugins subfolder of its settings folder.
  const std::wstring plugins = dir + L"\\Plugins";
  const DWORD attr = GetFileAttributesW(plugins.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) dir = plugins;
  return dir + L"\\out_crossfade.ini";
}

void ensureSettings() {
  if (g_settingsLoaded) return;
  g_settings = xf::loadSettingsIni(iniPath());
  g_settingsLoaded = true;
}

void ensureEngine() {
  ensureSettings();
  if (g_engine) return;
  g_engine = std::make_unique<xf::Engine>(g_settings);
  g_output = std::make_unique<xf::WasapiOutput>(*g_engine);
  g_output->setDevice(xf::utf8ToWide(g_settings.deviceId), g_settings.deviceBufferMs);
}

void Config(HWND parent) {
  ensureSettings();
  xf::Settings s = g_settings;
  const std::wstring status =
      L"Last crossfade: " + xf::utf8ToWide(xf::describeTransition(g_engine ? g_engine->lastTransition() : xf::TransitionInfo()));
  if (!xf::showConfigDialog(g_instance, parent, s, status)) return;
  g_settings = s;
  xf::saveSettingsIni(iniPath(), s);
  if (g_engine) {
    g_engine->setSettings(s);
    g_output->setDevice(xf::utf8ToWide(s.deviceId), s.deviceBufferMs);
  }
}

void About(HWND parent) {
  const std::wstring text = xf::utf8ToWide(
      "Crossfade Output v" XF_VERSION "\n\n"
      "Crossfades with beat sync and fade-out detection, gapless playback\n"
      "and smooth fades for Winamp, through WASAPI with high-quality resampling.\n\n"
      "Free and open source (MIT License). No ads, no tracking, no network access.");
  MessageBoxW(parent, text.c_str(), L"About Crossfade Output", MB_OK | MB_ICONINFORMATION);
}

void Init() { ensureEngine(); }

void Quit() {
  if (g_output) g_output->shutdown(kQuitDrainMs);
  g_output.reset();
  g_engine.reset();
}

int Open(int samplerate, int numchannels, int bitspersamp, int bufferlenms, int prebufferms) {
  ensureEngine();
  g_output->ensureRunning(kOpenTimeoutMs);
  return g_engine->open(samplerate, numchannels, bitspersamp, bufferlenms, prebufferms);
}

void Close() {
  if (g_engine) g_engine->close();
}

int Write(char* buf, int len) { return g_engine ? g_engine->write(buf, len) : 0; }
int CanWrite() { return g_engine ? g_engine->canWrite() : 0; }
int IsPlaying() { return g_engine ? g_engine->isPlaying() : 0; }
int Pause(int pause) { return g_engine ? g_engine->pause(pause) : 0; }

void SetVolume(int volume) {
  if (g_engine) g_engine->setVolume(volume);
}

void SetPan(int pan) {
  if (g_engine) g_engine->setPan(pan);
}

void Flush(int t) {
  if (g_engine) g_engine->flush(t);
}

int GetOutputTime() { return g_engine ? g_engine->outputTime() : 0; }
int GetWrittenTime() { return g_engine ? g_engine->writtenTime() : 0; }

Out_Module g_module = {
    OUT_VER, g_description, kModuleId, nullptr, nullptr,
    Config,  About,         Init,      Quit,    Open,
    Close,   Write,         CanWrite,  IsPlaying, Pause,
    SetVolume, SetPan,      Flush,     GetOutputTime, GetWrittenTime,
};

}  // namespace

extern "C" __declspec(dllexport) Out_Module* winampGetOutModule() { return &g_module; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_instance = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
