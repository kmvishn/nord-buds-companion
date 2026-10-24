#pragma once

#include <functional>
#include <vector>

// Preferences, stored under HKEY_CURRENT_USER\Software\NordBuds.
class Settings {
public:
    static Settings& get();

    // Start the tray service at sign-in (HKCU\...\CurrentVersion\Run).
    bool autostart() const;
    void set_autostart(bool on);

    // Tray icon shows the lowest earbud battery instead of the plain earbuds icon.
    bool tray_battery() const { return tray_battery_; }
    void set_tray_battery(bool on);

    void add_listener(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

private:
    Settings();
    void notify();

    bool tray_battery_ = true;
    std::vector<std::function<void()>> listeners_;
};
