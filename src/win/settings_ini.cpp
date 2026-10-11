#include <map>
#include <vector>

#include "win_support.h"

namespace xf {

namespace {
const wchar_t kSection[] = L"out_crossfade";
const wchar_t kMissing[] = L"\x7f<missing>";
}  // namespace

std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

std::string wideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

Settings loadSettingsIni(const std::wstring& path) {
  Settings s;
  std::map<std::string, std::string> found;
  std::vector<wchar_t> buf(2048);
  for (const auto& kv : s.toKeyValues()) {
    const std::wstring key = utf8ToWide(kv.first);
    GetPrivateProfileStringW(kSection, key.c_str(), kMissing, buf.data(), static_cast<DWORD>(buf.size()),
                             path.c_str());
    const std::wstring value(buf.data());
    if (value != kMissing) found[kv.first] = wideToUtf8(value);
  }
  s.fromKeyValues(found);
  return s;
}

void saveSettingsIni(const std::wstring& path, const Settings& s) {
  for (const auto& kv : s.toKeyValues()) {
    WritePrivateProfileStringW(kSection, utf8ToWide(kv.first).c_str(), utf8ToWide(kv.second).c_str(),
                               path.c_str());
  }
}

}  // namespace xf
