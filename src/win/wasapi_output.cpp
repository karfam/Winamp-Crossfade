#include "wasapi_output.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "engine.h"
#include "fifo.h"
#include "pcm.h"
#include "resampler.h"

#ifndef AUDCLNT_STREAMFLAGS_NOPERSIST
#define AUDCLNT_STREAMFLAGS_NOPERSIST 0x00080000
#endif

namespace xf {

namespace {

// Interface ids, spelled out to avoid depending on uuid libraries.
const CLSID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIidIAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIidIAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const IID kIidIMMNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
const PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

constexpr ERole kRole = eMultimedia;
constexpr DWORD kIdleCloseMs = 3000;
constexpr int kNullClockRetryMs = 5000;
constexpr int kNullClockMaxCatchUpMs = 3000;
constexpr int kNullClockRate = 48000;
constexpr int kAdapterChunk = 256;

template <class T>
void release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

unsigned long long nowMs() {
  using namespace std::chrono;
  return static_cast<unsigned long long>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Scoped COM initialisation that tolerates an already-initialised thread.
struct ComScope {
  HRESULT hr;
  explicit ComScope(DWORD model) : hr(CoInitializeEx(nullptr, model)) {}
  ~ComScope() {
    if (SUCCEEDED(hr)) CoUninitialize();
  }
};

class NotificationClient final : public IMMNotificationClient {
 public:
  explicit NotificationClient(WasapiOutput* owner) : owner_(owner) {}

  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == kIidIMMNotificationClient) {
      *ppv = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == kRole) owner_->onDefaultDeviceChanged();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

 private:
  LONG refs_ = 1;
  WasapiOutput* owner_;
};

}  // namespace

struct WasapiOutput::Device {
  IMMDevice* device = nullptr;
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  HANDLE event = nullptr;
  UINT32 bufferFrames = 0;

  // Device format.
  int rate = 0, channels = 0, containerBytes = 0, validBits = 0;
  bool isFloat = false;

  // Engine -> device conversion when the engine mix format differs from the
  // device (the device changed while tracks were playing).
  int engineRate = 0, engineChannels = 0;
  std::unique_ptr<Resampler> resampler;
  AudioFifo adapted;
  std::vector<float> mix, engineBuf, mapped, resampled;

  bool adapting() const { return engineRate != rate || engineChannels != channels; }
};

WasapiOutput::WasapiOutput(Engine& engine) : engine_(engine), dev_(std::make_unique<Device>()) {}

WasapiOutput::~WasapiOutput() { shutdown(0); }

void WasapiOutput::setDevice(const std::wstring& id, int bufferMs) {
  std::lock_guard<std::mutex> lk(mu_);
  if (id == deviceId_ && bufferMs == bufferMs_) return;
  deviceId_ = id;
  bufferMs_ = bufferMs;
  if (running_) reopen_ = true;
}

void WasapiOutput::onDefaultDeviceChanged() {
  std::lock_guard<std::mutex> lk(mu_);
  if (deviceId_.empty() && running_) reopen_ = true;
}

bool WasapiOutput::ensureRunning(int timeoutMs) {
  std::unique_lock<std::mutex> lk(mu_);
  if (!thread_.joinable()) {
    quit_ = false;
    thread_ = std::thread(&WasapiOutput::threadMain, this);
  }
  openRequested_ = true;  // also keeps a running device from idling out
  cv_.notify_all();
  if (running_) return true;
  const unsigned long long gen = openedGeneration_;
  cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return openedGeneration_ != gen || quit_; });
  return engine_.configured();
}

void WasapiOutput::shutdown(int drainTimeoutMs) {
  {
    std::unique_lock<std::mutex> lk(mu_);
    if (!thread_.joinable()) return;
  }
  const unsigned long long deadline = nowMs() + static_cast<unsigned long long>(std::max(0, drainTimeoutMs));
  while (engine_.hasWork() && nowMs() < deadline) Sleep(10);
  {
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

// ---------------------------------------------------------------------------
// Render thread

void WasapiOutput::threadMain() {
  ComScope com(COINIT_MULTITHREADED);

  // Ask MMCSS for audio scheduling priority (Vista+).
  HANDLE mmcss = nullptr;
  using AvSetFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
  using AvRevertFn = BOOL(WINAPI*)(HANDLE);
  HMODULE avrt = LoadLibraryW(L"avrt.dll");
  if (avrt) {
    auto avSet = reinterpret_cast<AvSetFn>(reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")));
    DWORD task = 0;
    if (avSet) mmcss = avSet(L"Pro Audio", &task);
  }
  if (!mmcss) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidIMMDeviceEnumerator,
                   reinterpret_cast<void**>(&enumerator));
  NotificationClient* notify = nullptr;
  if (enumerator) {
    notify = new NotificationClient(this);
    if (FAILED(enumerator->RegisterEndpointNotificationCallback(notify))) release(notify);
  }

  for (;;) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait_for(lk, std::chrono::milliseconds(250), [&] { return quit_ || openRequested_; });
      if (quit_) break;
      if (!openRequested_ && !engine_.hasWork()) continue;
      openRequested_ = false;
      reopen_ = false;
      running_ = true;
    }
    const bool ok = enumerator && openDevice(enumerator);
    if (!ok && !engine_.configured()) engine_.setOutputFormat(kNullClockRate, 2);
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++openedGeneration_;
    }
    cv_.notify_all();

    if (ok) {
      nullClockActive_ = false;
      runDevice();
    } else {
      runNullClock();
    }
    closeDevice();
    engine_.setOutputLatencyMs(0.0);
    {
      std::lock_guard<std::mutex> lk(mu_);
      running_ = false;
    }
  }

  if (notify) {
    enumerator->UnregisterEndpointNotificationCallback(notify);
    release(notify);
  }
  release(enumerator);
  if (mmcss && avrt) {
    auto avRevert = reinterpret_cast<AvRevertFn>(reinterpret_cast<void*>(GetProcAddress(avrt, "AvRevertMmThreadCharacteristics")));
    if (avRevert) avRevert(mmcss);
  }
  if (avrt) FreeLibrary(avrt);
}

bool WasapiOutput::openDevice(void* enumeratorPtr) {
  auto* enumerator = static_cast<IMMDeviceEnumerator*>(enumeratorPtr);
  Device& d = *dev_;
  std::wstring id;
  int bufferMs;
  {
    std::lock_guard<std::mutex> lk(mu_);
    id = deviceId_;
    bufferMs = bufferMs_;
  }

  if (!id.empty()) enumerator->GetDevice(id.c_str(), &d.device);
  if (!d.device && FAILED(enumerator->GetDefaultAudioEndpoint(eRender, kRole, &d.device))) return false;
  if (FAILED(d.device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&d.client)))) {
    closeDevice();
    return false;
  }

  WAVEFORMATEX* wfx = nullptr;
  if (FAILED(d.client->GetMixFormat(&wfx)) || !wfx) {
    closeDevice();
    return false;
  }
  d.rate = static_cast<int>(wfx->nSamplesPerSec);
  d.channels = wfx->nChannels;
  d.containerBytes = wfx->nChannels ? wfx->nBlockAlign / wfx->nChannels : 0;
  d.validBits = wfx->wBitsPerSample;
  d.isFloat = wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
  if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
    d.isFloat = ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    if (ext->Samples.wValidBitsPerSample) d.validBits = ext->Samples.wValidBitsPerSample;
  }
  const bool supported = d.rate > 0 && d.channels > 0 &&
                         ((d.isFloat && d.containerBytes == 4) ||
                          (!d.isFloat && (d.containerBytes == 2 || d.containerBytes == 3 || d.containerBytes == 4)));
  HRESULT hr = E_FAIL;
  if (supported) {
    const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(bufferMs) * 10000;
    hr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                              duration, 0, wfx, nullptr);
  }
  CoTaskMemFree(wfx);
  if (FAILED(hr) || FAILED(d.client->GetBufferSize(&d.bufferFrames)) ||
      FAILED(d.client->GetService(kIidIAudioRenderClient, reinterpret_cast<void**>(&d.render)))) {
    closeDevice();
    return false;
  }
  d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!d.event || FAILED(d.client->SetEventHandle(d.event))) {
    closeDevice();
    return false;
  }

  // Mix in the device format when possible; otherwise convert.
  engine_.setOutputFormat(d.rate, std::min(d.channels, 8));
  d.engineRate = engine_.outputRate();
  d.engineChannels = engine_.outputChannels();
  d.resampler.reset();
  if (d.adapting()) {
    if (d.engineRate != d.rate) d.resampler = std::make_unique<Resampler>(d.engineRate, d.rate, d.channels);
    d.adapted.reset(d.channels, static_cast<size_t>(kAdapterChunk) * 4);
  }

  // Prime the buffer with real audio, then start.
  BYTE* data = nullptr;
  if (SUCCEEDED(d.render->GetBuffer(d.bufferFrames, &data))) {
    fill(data, d.bufferFrames);
    d.render->ReleaseBuffer(d.bufferFrames, 0);
  }
  if (FAILED(d.client->Start())) {
    closeDevice();
    return false;
  }
  return true;
}

void WasapiOutput::closeDevice() {
  Device& d = *dev_;
  if (d.client) d.client->Stop();
  release(d.render);
  release(d.client);
  release(d.device);
  if (d.event) {
    CloseHandle(d.event);
    d.event = nullptr;
  }
  d.bufferFrames = 0;
}

// Returns false when the device loop should end (quit, reopen, or idle).
bool WasapiOutput::keepRunning(unsigned long long& idleSince) {
  std::lock_guard<std::mutex> lk(mu_);
  if (quit_) return false;
  if (reopen_) {
    openRequested_ = true;
    return false;
  }
  const unsigned long long now = nowMs();
  if (openRequested_ || engine_.hasWork()) {
    openRequested_ = false;
    idleSince = now;
    return true;
  }
  return now - idleSince < kIdleCloseMs;
}

void WasapiOutput::runDevice() {
  Device& d = *dev_;
  unsigned long long idleSince = nowMs();
  while (keepRunning(idleSince)) {
    WaitForSingleObject(d.event, 200);
    UINT32 padding = 0;
    if (FAILED(d.client->GetCurrentPadding(&padding))) break;  // device lost
    const UINT32 avail = d.bufferFrames - padding;
    if (avail > 0) {
      BYTE* data = nullptr;
      if (FAILED(d.render->GetBuffer(avail, &data))) break;
      fill(data, avail);
      if (FAILED(d.render->ReleaseBuffer(avail, 0))) break;
    }
    engine_.setOutputLatencyMs((padding + avail) * 1000.0 / d.rate);
  }
  // If we stopped because of an error while there is music, come straight back.
  std::lock_guard<std::mutex> lk(mu_);
  if (!quit_ && engine_.hasWork()) openRequested_ = true;
}

void WasapiOutput::runNullClock() {
  // No usable device: consume audio in real time so Winamp keeps going, and
  // retry the device every few seconds.
  const int rate = engine_.outputRate();
  const int ch = engine_.outputChannels();
  if (rate <= 0) return;
  const unsigned long long entered = nowMs();
  if (!nullClockActive_ || entered - (nullClockStartMs_ + nullClockRendered_ * 1000 / rate) > kNullClockMaxCatchUpMs) {
    nullClockActive_ = true;
    nullClockStartMs_ = entered;
    nullClockRendered_ = 0;
  }
  std::vector<float> buf;
  unsigned long long idleSince = entered;
  while (keepRunning(idleSince) && nowMs() - entered < static_cast<unsigned long long>(kNullClockRetryMs)) {
    const long long due = static_cast<long long>((nowMs() - nullClockStartMs_) * rate / 1000);
    const int frames = static_cast<int>(std::min<long long>(due - nullClockRendered_, rate));
    if (frames > 0) {
      buf.resize(static_cast<size_t>(frames) * ch);
      engine_.render(buf.data(), frames);
      nullClockRendered_ += frames;
    }
    Sleep(10);
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (!quit_ && engine_.hasWork()) openRequested_ = true;
  else nullClockActive_ = false;  // idle: start a fresh clock next time
}

bool WasapiOutput::fill(unsigned char* data, unsigned frames) {
  Device& d = *dev_;
  const size_t samples = static_cast<size_t>(frames) * d.channels;
  d.mix.resize(samples);

  if (!d.adapting()) {
    engine_.render(d.mix.data(), static_cast<int>(frames));
  } else {
    while (d.adapted.size() < frames) {
      d.engineBuf.resize(static_cast<size_t>(kAdapterChunk) * d.engineChannels);
      engine_.render(d.engineBuf.data(), kAdapterChunk);
      const float* src = d.engineBuf.data();
      if (d.engineChannels != d.channels) {
        d.mapped.resize(static_cast<size_t>(kAdapterChunk) * d.channels);
        mapChannels(src, d.engineChannels, d.mapped.data(), d.channels, kAdapterChunk);
        src = d.mapped.data();
      }
      if (d.resampler) {
        d.resampled.clear();
        d.resampler->process(src, kAdapterChunk, d.resampled);
        d.adapted.push(d.resampled.data(), d.resampled.size() / d.channels);
      } else {
        d.adapted.push(src, kAdapterChunk);
      }
    }
    for (unsigned i = 0; i < frames; ++i)
      std::memcpy(&d.mix[static_cast<size_t>(i) * d.channels], d.adapted.frame(i), d.channels * sizeof(float));
    d.adapted.discard(frames);
  }

  // Convert to the device sample format.
  const float* s = d.mix.data();
  if (d.isFloat) {
    std::memcpy(data, s, samples * sizeof(float));
    return true;
  }
  auto clamp1 = [](float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); };
  switch (d.containerBytes) {
    case 2:
      for (size_t i = 0; i < samples; ++i) {
        const int16_t v = static_cast<int16_t>(std::lrint(clamp1(s[i]) * 32767.0f));
        std::memcpy(data + i * 2, &v, 2);
      }
      break;
    case 3:
      for (size_t i = 0; i < samples; ++i) {
        const int32_t v = static_cast<int32_t>(std::lrint(clamp1(s[i]) * 8388607.0f));
        data[i * 3 + 0] = static_cast<unsigned char>(v & 0xFF);
        data[i * 3 + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
        data[i * 3 + 2] = static_cast<unsigned char>((v >> 16) & 0xFF);
      }
      break;
    case 4:
      for (size_t i = 0; i < samples; ++i) {
        // 24-in-32 or full 32-bit: compute at 24-bit precision, left-justified.
        const int32_t v24 = static_cast<int32_t>(std::lrint(clamp1(s[i]) * 8388607.0f));
        const int32_t v = static_cast<int32_t>(static_cast<uint32_t>(v24) << 8);
        std::memcpy(data + i * 4, &v, 4);
      }
      break;
    default:
      std::memset(data, 0, samples * d.containerBytes);
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------

std::vector<AudioDevice> WasapiOutput::listDevices() {
  std::vector<AudioDevice> result;
  ComScope com(COINIT_APARTMENTTHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidIMMDeviceEnumerator,
                              reinterpret_cast<void**>(&enumerator))))
    return result;
  IMMDeviceCollection* devices = nullptr;
  if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
      IMMDevice* dev = nullptr;
      if (FAILED(devices->Item(i, &dev))) continue;
      AudioDevice info;
      LPWSTR id = nullptr;
      if (SUCCEEDED(dev->GetId(&id)) && id) {
        info.id = id;
        CoTaskMemFree(id);
      }
      IPropertyStore* props = nullptr;
      if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(kPkeyDeviceFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
          info.name = pv.pwszVal;
        PropVariantClear(&pv);
        release(props);
      }
      if (info.name.empty()) info.name = info.id;
      if (!info.id.empty()) result.push_back(std::move(info));
      release(dev);
    }
    release(devices);
  }
  release(enumerator);
  return result;
}

}  // namespace xf
