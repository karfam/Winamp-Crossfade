#pragma once

#include <windows.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xf {

class Engine;

struct AudioDevice {
  std::wstring id;
  std::wstring name;
};

// Pulls audio from the Engine on its own thread and plays it through a WASAPI
// shared-mode endpoint (event driven, MMCSS "Pro Audio"). The device is
// opened on demand, closed after a few idle seconds, follows the system
// default device, and recovers when the device disappears. Without any usable
// device it keeps time with a silent clock so playback never stalls.
class WasapiOutput {
 public:
  explicit WasapiOutput(Engine& engine);
  ~WasapiOutput();
  WasapiOutput(const WasapiOutput&) = delete;
  WasapiOutput& operator=(const WasapiOutput&) = delete;

  // Empty id = system default device. Reopens the device if it is running.
  void setDevice(const std::wstring& id, int bufferMs);

  // Starts the render thread if needed and waits until the device is open
  // (or known to be unavailable), so the engine has its mix format.
  bool ensureRunning(int timeoutMs);

  // Lets fades finish for up to drainTimeoutMs, then stops the thread.
  void shutdown(int drainTimeoutMs);

  static std::vector<AudioDevice> listDevices();

  // Called from the device notification callback.
  void onDefaultDeviceChanged();

 private:
  struct Device;

  void threadMain();
  bool openDevice(void* enumerator);
  void closeDevice();
  void runDevice();
  void runNullClock();
  bool fill(unsigned char* data, unsigned frames);
  bool keepRunning(unsigned long long& idleSince);

  Engine& engine_;
  std::unique_ptr<Device> dev_;

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool quit_ = false;
  bool openRequested_ = false;
  bool reopen_ = false;
  bool running_ = false;
  unsigned long long openedGeneration_ = 0;

  // Silent clock used when no device can be opened. It survives device retry
  // attempts so playback time does not lose the time spent probing.
  bool nullClockActive_ = false;
  unsigned long long nullClockStartMs_ = 0;
  long long nullClockRendered_ = 0;
  std::wstring deviceId_;
  int bufferMs_ = 40;
};

}  // namespace xf
