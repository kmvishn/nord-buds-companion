#pragma once

#include <windows.h>

#include <functional>
#include <string>

class Buds;

// Notification-area icon: battery at a glance, tooltip, quick menu and low-battery notices.
class Tray {
public:
    struct Actions {
        std::function<void()> show;
        std::function<void()> quit;
    };

    Tray(Buds& buds, Actions actions);
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    void update();
    void notify(const std::wstring& title, const std::wstring& body);
    void clear_notification();

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void add();
    void show_menu(int x, int y);

    Buds& buds_;
    Actions actions_;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    std::wstring icon_key_;
    std::wstring tip_;
    UINT taskbar_created_ = 0;
    bool added_ = false;
};
