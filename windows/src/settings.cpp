#include "settings.h"

#include <windows.h>

#include <string>

namespace {

constexpr wchar_t kKey[] = L"Software\\NordBuds";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"NordBuds";

bool read_dword(const wchar_t* name, DWORD* out) {
    DWORD size = sizeof *out;
    return RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, out, &size) == ERROR_SUCCESS;
}

void write_dword(const wchar_t* name, DWORD value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
    RegCloseKey(key);
}

}  // namespace

Settings& Settings::get() {
    static Settings settings;
    return settings;
}

Settings::Settings() {
    DWORD v = 0;
    if (read_dword(L"TrayBattery", &v)) tray_battery_ = v != 0;
}

void Settings::notify() {
    for (auto& fn : listeners_) fn();
}

bool Settings::autostart() const {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void Settings::set_autostart(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(path, n) + L"\" --background";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       DWORD((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
    notify();
}

void Settings::set_tray_battery(bool on) {
    if (tray_battery_ == on) return;
    tray_battery_ = on;
    write_dword(L"TrayBattery", on ? 1 : 0);
    notify();
}
