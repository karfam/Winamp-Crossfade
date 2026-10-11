#include <vector>

#include "resource.h"
#include "wasapi_output.h"
#include "win_support.h"

namespace xf {

namespace {

struct DialogState {
  Settings* settings;
  std::vector<AudioDevice> devices;  // combo item i + 1 -> devices[i]
  std::wstring status;
};

void setInt(HWND h, int id, int v) { SetDlgItemInt(h, id, static_cast<UINT>(v), FALSE); }

int getInt(HWND h, int id, int fallback) {
  BOOL ok = FALSE;
  const UINT v = GetDlgItemInt(h, id, &ok, FALSE);
  return ok ? static_cast<int>(v) : fallback;
}

void setCheck(HWND h, int id, bool v) { CheckDlgButton(h, id, v ? BST_CHECKED : BST_UNCHECKED); }
bool getCheck(HWND h, int id) { return IsDlgButtonChecked(h, id) == BST_CHECKED; }

void updateEnabled(HWND h) {
  const bool xfade = getCheck(h, IDC_XFADE);
  for (int id : {IDC_XFADE_MS, IDC_XFADE_MS_LABEL, IDC_TRIM_TAIL, IDC_TRIM_HEAD, IDC_SILENCE_DB, IDC_SILENCE_LABEL,
                 IDC_SMART_FADE, IDC_BEAT_SYNC})
    EnableWindow(GetDlgItem(h, id), xfade);
}

void populate(HWND h, DialogState& st, const Settings& s) {
  setCheck(h, IDC_XFADE, s.crossfade);
  setInt(h, IDC_XFADE_MS, s.crossfadeMs);
  SendDlgItemMessageW(h, IDC_CURVE, CB_SETCURSEL, static_cast<WPARAM>(s.curve), 0);
  setCheck(h, IDC_TRIM_TAIL, s.trimTail);
  setCheck(h, IDC_TRIM_HEAD, s.trimHead);
  setInt(h, IDC_SILENCE_DB, -s.silenceThresholdDb);
  setCheck(h, IDC_SMART_FADE, s.smartFadeOut);
  setCheck(h, IDC_BEAT_SYNC, s.beatSync);
  setInt(h, IDC_FADE_START, s.fadeStartMs);
  setInt(h, IDC_FADE_STOP, s.fadeStopMs);
  setInt(h, IDC_FADE_PAUSE, s.fadePauseMs);
  setInt(h, IDC_FADE_SEEK, s.fadeSeekMs);
  setInt(h, IDC_BUFFER_MS, s.bufferMs);
  setInt(h, IDC_DEVICE_BUFFER_MS, s.deviceBufferMs);
  setCheck(h, IDC_LIMITER, s.softLimiter);
  setCheck(h, IDC_VOLUME, s.useWinampVolume);

  const std::wstring id = utf8ToWide(s.deviceId);
  int sel = 0;
  for (size_t i = 0; i < st.devices.size(); ++i)
    if (st.devices[i].id == id) sel = static_cast<int>(i) + 1;
  if (!id.empty() && sel == 0) {
    // Configured device is not connected right now; keep it selectable.
    st.devices.push_back({id, L"(not connected) " + id});
    SendDlgItemMessageW(h, IDC_DEVICE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(st.devices.back().name.c_str()));
    sel = static_cast<int>(st.devices.size());
  }
  SendDlgItemMessageW(h, IDC_DEVICE, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
  updateEnabled(h);
}

void collect(HWND h, const DialogState& st, Settings& s) {
  s.crossfade = getCheck(h, IDC_XFADE);
  s.crossfadeMs = getInt(h, IDC_XFADE_MS, s.crossfadeMs);
  const LRESULT curve = SendDlgItemMessageW(h, IDC_CURVE, CB_GETCURSEL, 0, 0);
  if (curve >= 0) s.curve = static_cast<Curve>(curve);
  s.trimTail = getCheck(h, IDC_TRIM_TAIL);
  s.trimHead = getCheck(h, IDC_TRIM_HEAD);
  s.silenceThresholdDb = -getInt(h, IDC_SILENCE_DB, -s.silenceThresholdDb);
  s.smartFadeOut = getCheck(h, IDC_SMART_FADE);
  s.beatSync = getCheck(h, IDC_BEAT_SYNC);
  s.fadeStartMs = getInt(h, IDC_FADE_START, s.fadeStartMs);
  s.fadeStopMs = getInt(h, IDC_FADE_STOP, s.fadeStopMs);
  s.fadePauseMs = getInt(h, IDC_FADE_PAUSE, s.fadePauseMs);
  s.fadeSeekMs = getInt(h, IDC_FADE_SEEK, s.fadeSeekMs);
  s.bufferMs = getInt(h, IDC_BUFFER_MS, s.bufferMs);
  s.deviceBufferMs = getInt(h, IDC_DEVICE_BUFFER_MS, s.deviceBufferMs);
  s.softLimiter = getCheck(h, IDC_LIMITER);
  s.useWinampVolume = getCheck(h, IDC_VOLUME);
  const LRESULT dev = SendDlgItemMessageW(h, IDC_DEVICE, CB_GETCURSEL, 0, 0);
  if (dev <= 0 || static_cast<size_t>(dev) > st.devices.size()) s.deviceId.clear();
  else s.deviceId = wideToUtf8(st.devices[static_cast<size_t>(dev) - 1].id);
  s.sanitize();
}

INT_PTR CALLBACK dialogProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<DialogState*>(lp);
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
      for (int c = 0; c < kCurveCount; ++c) {
        const std::wstring name = utf8ToWide(curveName(static_cast<Curve>(c)));
        SendDlgItemMessageW(h, IDC_CURVE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
      }
      SendDlgItemMessageW(h, IDC_DEVICE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"System default device"));
      for (const auto& d : st->devices)
        SendDlgItemMessageW(h, IDC_DEVICE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name.c_str()));
      for (int id : {IDC_XFADE_MS, IDC_FADE_START, IDC_FADE_STOP, IDC_FADE_PAUSE, IDC_BUFFER_MS})
        SendDlgItemMessageW(h, id, EM_SETLIMITTEXT, 5, 0);
      for (int id : {IDC_SILENCE_DB, IDC_FADE_SEEK, IDC_DEVICE_BUFFER_MS})
        SendDlgItemMessageW(h, id, EM_SETLIMITTEXT, 4, 0);
      populate(h, *st, *st->settings);
      SetDlgItemTextW(h, IDC_LAST_INFO, st->status.c_str());
      return TRUE;
    }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_XFADE:
          updateEnabled(h);
          return TRUE;
        case IDC_DEFAULTS: {
          Settings defaults;
          populate(h, *st, defaults);
          return TRUE;
        }
        case IDOK:
          collect(h, *st, *st->settings);
          EndDialog(h, IDOK);
          return TRUE;
        case IDCANCEL:
          EndDialog(h, IDCANCEL);
          return TRUE;
      }
      break;
  }
  return FALSE;
}

}  // namespace

bool showConfigDialog(HINSTANCE instance, HWND parent, Settings& s, const std::wstring& status) {
  DialogState st{&s, WasapiOutput::listDevices(), status};
  const INT_PTR r = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), parent, dialogProc,
                                    reinterpret_cast<LPARAM>(&st));
  return r == IDOK;
}

}  // namespace xf
