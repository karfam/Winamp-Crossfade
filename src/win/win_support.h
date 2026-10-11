#pragma once

#include <windows.h>

#include <string>

#include "settings.h"

namespace xf {

std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);

Settings loadSettingsIni(const std::wstring& path);
void saveSettingsIni(const std::wstring& path, const Settings& s);

// Modal settings dialog. Returns true and updates `s` when the user presses OK.
bool showConfigDialog(HINSTANCE instance, HWND parent, Settings& s);

}  // namespace xf
