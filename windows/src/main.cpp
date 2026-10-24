#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "buds.h"
#include "ipc.h"
#include "model.h"
#include "settings.h"
#include "tray.h"
#include "util.h"
#include "window.h"

namespace {

constexpr UINT kMsgCommand = WM_APP + 1;

// Where a command's output goes: the command line tool's reply window, or nowhere.
struct Client {
    HWND hwnd = nullptr;
};

struct Waiter {
    Client cl;
    std::function<bool()> done;
    std::function<int(Client)> then;
    std::function<std::string()> timeout_message;
    UINT_PTR timer = 0;
};

struct PendingCommand {
    Client cl;
    std::vector<std::string> args;
};

struct App {
    HWND hwnd = nullptr;  // service window: commands from nordbuds.com and waiter timeouts
    std::unique_ptr<Buds> buds;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<Tray> tray;
    std::vector<Waiter*> waiters;
    UINT_PTR next_timer = 1;
    bool low_warned[3] = {};
};

App app;

void print(const Client& c, const std::string& s) {
    if (!c.hwnd) return;
    std::string text = s + "\n";
    ipc::send(c.hwnd, ipc::kOut, text.data(), DWORD(text.size()));
}

void print_error(const Client& c, const std::string& s) {
    if (!c.hwnd) return;
    std::string text = "nordbuds: " + s + "\n";
    ipc::send(c.hwnd, ipc::kErr, text.data(), DWORD(text.size()));
}

void finish(const Client& c, int code) {
    if (c.hwnd) ipc::send(c.hwnd, ipc::kExit, &code, sizeof code);
}

// then() returns an exit code, or -1 when it handed cl to another wait.
void wait_for(Client cl, std::function<bool()> done, std::function<int(Client)> then,
              std::function<std::string()> timeout_message, UINT seconds) {
    if (done()) {
        int code = then(cl);
        if (code >= 0) finish(cl, code);
        return;
    }
    auto* w = new Waiter{cl, std::move(done), std::move(then), std::move(timeout_message)};
    w->timer = app.next_timer++;
    SetTimer(app.hwnd, w->timer, seconds * 1000, nullptr);
    app.waiters.push_back(w);
}

void on_waiter_timeout(UINT_PTR timer) {
    KillTimer(app.hwnd, timer);
    auto it = std::find_if(app.waiters.begin(), app.waiters.end(), [timer](Waiter* w) { return w->timer == timer; });
    if (it == app.waiters.end()) return;
    Waiter* w = *it;
    app.waiters.erase(it);
    print_error(w->cl, w->timeout_message());
    finish(w->cl, 1);
    delete w;
}

void check_waiters() {
    while (true) {
        auto it = std::find_if(app.waiters.begin(), app.waiters.end(), [](Waiter* w) { return w->done(); });
        if (it == app.waiters.end()) return;
        Waiter* w = *it;
        app.waiters.erase(it);
        KillTimer(app.hwnd, w->timer);
        int code = w->then(w->cl);
        if (code >= 0) finish(w->cl, code);
        delete w;
    }
}

std::string percent(const proto::BatteryPart& b) {
    if (b.level < 0) return "–";
    return std::to_string(b.level) + "%" + (b.charging ? " (charging)" : "");
}

std::string battery_text(const BudsState& st) {
    std::string s = "left " + percent(st.battery.left) + "   right " + percent(st.battery.right) + "   case ";
    s += st.battery.box.level < 0 ? "– (open the lid to read)" : percent(st.battery.box);
    return s;
}

std::string eq_text(int id) {
    if (const auto* p = model::eq_by_id(id)) return p->name;
    return id < 0 ? "unknown" : "preset " + std::to_string(id);
}

std::string game_text(int v) {
    return v < 0 ? "unknown" : v ? "on" : "off";
}

std::string touch_text(const BudsState& st) {
    std::string out;
    for (const auto& slot : model::gesture_slots()) {
        std::string left = "–", right = "–";
        for (const auto& e : st.gestures) {
            if (e.act != slot.act) continue;
            if (e.dev == model::kLeft) left = model::function_name(e.fn);
            if (e.dev == model::kRight) right = model::function_name(e.fn);
        }
        char line[160];
        std::snprintf(line, sizeof line, "  %-15s left: %-16s right: %s\n", slot.name, left.c_str(), right.c_str());
        out += line;
    }
    if (!out.empty()) out.pop_back();
    return out;
}

std::string link_text(const Buds& buds) {
    const auto& st = buds.state();
    if (!st.radio) return "Bluetooth is off";
    if (st.disconnecting) return "disconnecting";
    switch (st.link) {
    case LinkState::NoDevice: return "no paired Nord Buds found";
    case LinkState::Disconnected: return "not connected";
    case LinkState::Connecting: return "connecting";
    case LinkState::Connected: return buds.ready() ? "connected" : "reading settings";
    }
    return "";
}

std::string not_ready_message() {
    const auto& st = app.buds->state();
    std::string s = "earbuds not reachable (" + link_text(*app.buds) + ")";
    if (!st.last_error.empty()) s += ": " + st.last_error;
    return s;
}

// Waits for the control channel, then runs fn. Fails fast when Bluetooth is not connected.
void with_buds(Client cl, std::function<int(Client)> fn) {
    const auto& st = app.buds->state();
    if (!st.radio) {
        print_error(cl, "Bluetooth is off. Turn it on in Settings → Bluetooth & devices.");
        finish(cl, 1);
        return;
    }
    if (st.link == LinkState::NoDevice) {
        print_error(cl, "no paired OnePlus Nord Buds found. Pair them in Settings → Bluetooth & devices.");
        finish(cl, 1);
        return;
    }
    if (!st.bt_connected || st.disconnecting) {
        print_error(cl, "earbuds are not connected. Take them out of the case, or run: nordbuds connect");
        finish(cl, 1);
        return;
    }
    wait_for(cl, [] { return app.buds->ready(); }, std::move(fn), not_ready_message, 20);
}

// Sends a write, then waits for the read-back to confirm it (or for the buds to refuse it).
void confirm(Client cl, std::function<bool()> applied, std::string success) {
    unsigned serial = app.buds->state().error_serial;
    wait_for(
        cl, [applied, serial] { return applied() || app.buds->state().error_serial != serial || !app.buds->ready(); },
        [applied, success](Client c) {
            if (applied()) {
                print(c, success);
                return 0;
            }
            print_error(c, app.buds->ready() ? app.buds->state().last_error : not_ready_message());
            return 1;
        },
        [] { return std::string("the earbuds did not confirm the change"); }, 6);
}

void quit() {
    PostQuitMessage(0);
}

void open_bluetooth_settings() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
}

void show_window() {
    if (!app.window) app.window = std::make_unique<MainWindow>(*app.buds, MainWindow::Actions{quit, open_bluetooth_settings});
    app.window->present();
}

void withdraw_low_battery(int i) {
    app.low_warned[i] = false;
    if (app.tray) app.tray->clear_notification();
}

void warn_low_battery() {
    if (!app.buds->ready()) {
        // A low-battery notice must not outlive the connection it describes.
        for (int i = 0; i < 3; i++)
            if (app.low_warned[i]) withdraw_low_battery(i);
        return;
    }
    const auto& b = app.buds->state().battery;
    const proto::BatteryPart* parts[3] = {&b.left, &b.right, &b.box};
    const wchar_t* names[3] = {L"Left earbud", L"Right earbud", L"Charging case"};
    for (int i = 0; i < 3; i++) {
        const auto& p = *parts[i];
        if (p.level < 0) continue;
        if (p.level > 20 || p.charging) {
            if (app.low_warned[i]) withdraw_low_battery(i);
        } else if (p.level <= 15 && !app.low_warned[i]) {
            app.low_warned[i] = true;
            if (app.tray)
                app.tray->notify(L"Earbuds battery low", std::wstring(names[i]) + L" is at " + std::to_wstring(p.level) + L"%");
        }
    }
}

void on_buds_changed() {
    if (app.window) app.window->refresh();
    if (app.tray) app.tray->update();
    warn_low_battery();
    check_waiters();
}

void run_command(Client cl, const std::vector<std::string>& args) {
    const std::string& cmd = args[0];
    auto usage_error = [cl](const std::string& msg) {
        print_error(cl, msg + " (see: nordbuds help)");
        finish(cl, 2);
    };

    if (cmd == "show") {
        show_window();
        finish(cl, 0);
    } else if (cmd == "quit") {
        print(cl, "Stopping Nord Buds.");
        finish(cl, 0);
        quit();
    } else if (cmd == "status") {
        with_buds(cl, [](Client c) {
            const auto& st = app.buds->state();
            print(c, st.name + " (" + st.address + ")");
            print(c, "Link:        " + link_text(*app.buds));
            print(c, "Battery:     " + battery_text(st));
            print(c, "Sound:       " + eq_text(st.eq));
            print(c, "Game mode:   " + game_text(st.game_mode));
            print(c, "Firmware:    " + (st.firmware.summary.empty() ? std::string("–") : st.firmware.summary));
            print(c, "Touch controls:");
            print(c, touch_text(st));
            return 0;
        });
    } else if (cmd == "battery") {
        with_buds(cl, [](Client c) {
            print(c, battery_text(app.buds->state()));
            return 0;
        });
    } else if (cmd == "eq") {
        const model::EqPreset* preset = nullptr;
        if (args.size() > 1 && !(preset = model::eq_by_key(args[1])))
            return usage_error("unknown sound profile '" + args[1] + "'");
        with_buds(cl, [preset](Client c) {
            if (!preset) {
                print(c, "Sound profile: " + eq_text(app.buds->state().eq));
                return 0;
            }
            int id = preset->id;
            if (app.buds->state().eq == id) {
                print(c, std::string("Sound profile is already ") + preset->name);
                return 0;
            }
            if (!app.buds->set_eq(id)) {
                print_error(c, "these earbuds do not accept sound profile changes");
                return 1;
            }
            confirm(c, [id] { return app.buds->state().eq == id; }, std::string("Sound profile set to ") + preset->name);
            return -1;
        });
    } else if (cmd == "game") {
        int want = -1;
        if (args.size() > 1) {
            if (args[1] == "on") want = 1;
            else if (args[1] == "off") want = 0;
            else return usage_error("game takes on or off");
        }
        with_buds(cl, [want](Client c) {
            if (want < 0 || app.buds->state().game_mode == want) {
                print(c, "Game mode: " + game_text(app.buds->state().game_mode));
                return 0;
            }
            if (!app.buds->set_game_mode(want == 1)) {
                print_error(c, "these earbuds do not accept game mode changes");
                return 1;
            }
            confirm(c, [want] { return app.buds->state().game_mode == want; },
                    std::string("Game mode turned ") + (want ? "on" : "off"));
            return -1;
        });
    } else if (cmd == "find") {
        bool on = true;
        if (args.size() > 1) {
            if (args[1] == "off" || args[1] == "stop") on = false;
            else if (args[1] != "on" && args[1] != "start") return usage_error("find takes on or off");
        }
        with_buds(cl, [on](Client c) {
            if (!app.buds->set_find(on)) {
                print_error(c, "these earbuds cannot ring");
                return 1;
            }
            print(c, on ? "Ringing both earbuds. Stop with: nordbuds find off" : "Stopped ringing.");
            return 0;
        });
    } else if (cmd == "touch") {
        if (args.size() == 1) {
            with_buds(cl, [](Client c) {
                print(c, touch_text(app.buds->state()));
                return 0;
            });
            return;
        }
        if (args.size() != 4) return usage_error("touch takes <side> <gesture> <action>");
        std::vector<uint8_t> devs;
        if (args[1] == "left") devs = {model::kLeft};
        else if (args[1] == "right") devs = {model::kRight};
        else if (args[1] == "both") devs = {model::kLeft, model::kRight};
        else return usage_error("side must be left, right or both");
        const model::GestureSlot* slot = model::slot_by_key(args[2]);
        if (!slot) return usage_error("gesture must be single, double, triple or hold");
        auto fn = model::function_by_key(args[3]);
        if (!fn || std::find(slot->options.begin(), slot->options.end(), *fn) == slot->options.end()) {
            std::string allowed;
            for (uint8_t o : slot->options) allowed += " " + model::function_key(o);
            return usage_error(std::string(slot->name) + " can be set to:" + allowed);
        }
        uint8_t value = *fn;
        with_buds(cl, [devs, slot, value](Client c) {
            auto applied = [devs, slot, value] {
                const auto& table = app.buds->state().gestures;
                for (uint8_t d : devs) {
                    bool ok = false;
                    for (const auto& e : table)
                        if (e.dev == d && e.act == slot->act) ok = e.fn == value;
                    if (!ok) return false;
                }
                return true;
            };
            if (applied()) {
                print(c, "Already set.");
                return 0;
            }
            if (!app.buds->set_gesture(devs, slot->act, value)) {
                print_error(c, "the earbuds did not accept that touch control");
                return 1;
            }
            confirm(c, applied, std::string(slot->name) + " set to " + model::function_name(value));
            return -1;
        });
    } else if (cmd == "tray") {
        if (args.size() == 2) {
            if (args[1] == "battery") Settings::get().set_tray_battery(true);
            else if (args[1] == "icon") Settings::get().set_tray_battery(false);
            else return usage_error("tray takes icon or battery");
        } else if (args.size() != 1) {
            return usage_error("tray takes icon or battery");
        }
        print(cl, std::string("Tray icon shows: ") + (Settings::get().tray_battery() ? "battery" : "icon"));
        finish(cl, 0);
    } else if (cmd == "autostart") {
        if (args.size() == 2) {
            if (args[1] == "on") Settings::get().set_autostart(true);
            else if (args[1] == "off") Settings::get().set_autostart(false);
            else return usage_error("autostart takes on or off");
        } else if (args.size() != 1) {
            return usage_error("autostart takes on or off");
        }
        print(cl, std::string("Start with Windows: ") + (Settings::get().autostart() ? "on" : "off"));
        finish(cl, 0);
    } else if (cmd == "connect" || cmd == "disconnect") {
        bool want = cmd == "connect";
        if (app.buds->state().link == LinkState::NoDevice) {
            print_error(cl, "no paired OnePlus Nord Buds found");
            finish(cl, 1);
            return;
        }
        if (want) app.buds->bluetooth_connect();
        else app.buds->bluetooth_disconnect();
        wait_for(
            cl,
            [want] {
                const auto& st = app.buds->state();
                return want ? st.bt_connected && !st.disconnecting : !st.bt_connected;
            },
            [want](Client c) {
                print(c, want ? "Connected." : "Disconnected.");
                return 0;
            },
            [] { return "Bluetooth did not respond: " + app.buds->state().last_error; }, 20);
    } else {
        usage_error("unknown command '" + cmd + "'");
    }
}

LRESULT CALLBACK service_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COPYDATA: {
        // Copy the command out and run it after this SendMessage returns, so the sender is not blocked.
        auto* cmd = new PendingCommand;
        if (!ipc::unpack(reinterpret_cast<const COPYDATASTRUCT*>(lp), &cmd->cl.hwnd, &cmd->args) || cmd->args.empty()) {
            delete cmd;
            return FALSE;
        }
        PostMessageW(hwnd, kMsgCommand, 0, reinterpret_cast<LPARAM>(cmd));
        return TRUE;
    }
    case kMsgCommand: {
        auto* cmd = reinterpret_cast<PendingCommand*>(lp);
        run_command(cmd->cl, cmd->args);
        delete cmd;
        return 0;
    }
    case WM_TIMER:
        on_waiter_timeout(UINT_PTR(wp));
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// Popup menus follow the dark theme only after these undocumented uxtheme exports are called
// (#135 SetPreferredAppMode, #136 FlushMenuThemes; Windows 10 1809 and later). Without them menus stay light.
void enable_dark_menus() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW os = {sizeof os};
    auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (!rtl_get_version || rtl_get_version(&os) != 0 || os.dwMajorVersion < 10 || os.dwBuildNumber < 17763) return;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    auto set_mode = reinterpret_cast<SetPreferredAppMode>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
    auto flush = reinterpret_cast<FlushMenuThemes>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
    if (set_mode) set_mode(1);  // AllowDark: follow the app theme
    if (flush) flush();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    std::string device;
    bool background = false;
    for (int i = 1; i < argc; i++) {
        std::string a = util::utf8(argv[i]);
        if (a == "--device" && i + 1 < argc) device = util::utf8(argv[++i]);
        else if (a == "--background" || a == "-b") background = true;
        else args.push_back(a);
    }
    LocalFree(argv);
    if (device.empty()) {
        if (const char* env = std::getenv("NORDBUDS_DEVICE")) device = env;
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, ipc::kMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: hand over the command (or open the window) and leave.
        HWND service = nullptr;
        for (int i = 0; i < 50 && !(service = ipc::find_service()); i++) Sleep(100);
        if (args.empty() && !background) args = {"show"};
        if (service && !args.empty()) {
            DWORD pid = 0;
            GetWindowThreadProcessId(service, &pid);
            AllowSetForegroundWindow(pid);
            auto data = ipc::pack(nullptr, args);
            ipc::send(service, ipc::kCommand, data.data(), DWORD(data.size()));
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    enable_dark_menus();

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = service_proc;
    wc.hInstance = inst;
    wc.lpszClassName = ipc::kServiceClass;
    RegisterClassExW(&wc);
    app.hwnd = CreateWindowExW(0, ipc::kServiceClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);

    app.buds = std::make_unique<Buds>(device);
    app.buds->add_listener(on_buds_changed);
    Settings::get().add_listener([] {
        if (app.tray) app.tray->update();
        if (app.window) app.window->refresh();
    });
    app.tray = std::make_unique<Tray>(*app.buds, Tray::Actions{show_window, quit});
    app.buds->start();
    if (!args.empty()) run_command(Client{}, args);
    else if (!background) show_window();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (Waiter* w : app.waiters) {
        print_error(w->cl, "Nord Buds is shutting down");
        finish(w->cl, 1);
        delete w;
    }
    app.waiters.clear();
    app.window.reset();
    app.tray.reset();
    app.buds.reset();
    DestroyWindow(app.hwnd);
    CoUninitialize();
    WSACleanup();
    if (mutex) CloseHandle(mutex);
    return 0;
}
