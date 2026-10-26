#include <gtk/gtk.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "api.h"
#include "buds.h"
#include "indicator.h"
#include "model.h"
#include "settings.h"
#include "window.h"

namespace {

constexpr const char* kAppId = "com.nordbuds.Companion";

constexpr const char* kUsage = R"(Usage: nordbuds [--device AA:BB:CC:DD:EE:FF] [command]

With no command, opens the window and keeps the top-bar indicator running.

Commands:
  --background, -b            run the top-bar indicator only (used at login)
  status                      everything the earbuds report
  battery                     left, right and case battery
  eq [balanced|bold|bass]     show or set the sound profile
  game [on|off]               show or set game mode (low latency)
  find [on|off]               ring the earbuds (default on); stops by itself after 60 s
  touch                       show touch controls
  touch <left|right|both> <single|double|triple|hold> <action>
                              actions: none, play-pause, previous, next,
                                       voice-assistant, game-mode, switch-device
  connect | disconnect        connect or disconnect Bluetooth audio
  panel                       show top-bar settings
  panel position <left|center|right>
  panel order <0-20>          0 = first item in that area
  panel label <icon|compact|buds|all>
                              icon only, lowest earbud, left+right, left+right+case
  show                        open the window
  quit                        stop the background service
)";

struct Waiter {
    GApplicationCommandLine* cl;
    std::function<bool()> done;
    std::function<int(GApplicationCommandLine*)> then;
    std::function<std::string()> timeout_message;
    guint timeout_id = 0;
};

struct App {
    GtkApplication* gtk = nullptr;
    std::unique_ptr<Buds> buds;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<Indicator> indicator;
    std::unique_ptr<ServiceApi> api;
    bool service = false;
    std::vector<Waiter*> waiters;
    int local_exit = -1;
    bool low_warned[3] = {};
};

App app;

void print(GApplicationCommandLine* cl, const std::string& s) {
    g_application_command_line_print(cl, "%s\n", s.c_str());
}

void print_error(GApplicationCommandLine* cl, const std::string& s) {
    g_application_command_line_printerr(cl, "nordbuds: %s\n", s.c_str());
}

void finish(GApplicationCommandLine* cl, int code) {
    g_application_command_line_set_exit_status(cl, code);
    if (!g_application_command_line_get_is_remote(cl)) app.local_exit = code;
    g_object_unref(cl);
    g_application_release(G_APPLICATION(app.gtk));
}

// then() returns an exit code, or -1 when it handed cl to another wait.
void wait_for(GApplicationCommandLine* cl, std::function<bool()> done,
              std::function<int(GApplicationCommandLine*)> then, std::function<std::string()> timeout_message,
              guint seconds) {
    if (done()) {
        int code = then(cl);
        if (code >= 0) finish(cl, code);
        return;
    }
    auto* w = new Waiter{cl, std::move(done), std::move(then), std::move(timeout_message)};
    w->timeout_id = g_timeout_add_seconds(seconds,
                                          +[](gpointer data) -> gboolean {
                                              auto* w = static_cast<Waiter*>(data);
                                              app.waiters.erase(std::remove(app.waiters.begin(), app.waiters.end(), w),
                                                                app.waiters.end());
                                              print_error(w->cl, w->timeout_message());
                                              finish(w->cl, 1);
                                              delete w;
                                              return G_SOURCE_REMOVE;
                                          },
                                          w);
    app.waiters.push_back(w);
}

void check_waiters() {
    while (true) {
        auto it = std::find_if(app.waiters.begin(), app.waiters.end(), [](Waiter* w) { return w->done(); });
        if (it == app.waiters.end()) return;
        Waiter* w = *it;
        app.waiters.erase(it);
        g_source_remove(w->timeout_id);
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
    if (!st.bluetooth) return "Bluetooth is off";
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
void with_buds(GApplicationCommandLine* cl, std::function<int(GApplicationCommandLine*)> fn) {
    const auto& st = app.buds->state();
    if (!st.bluetooth) {
        print_error(cl, "Bluetooth is off (the bluetooth service is not running)");
        finish(cl, 1);
        return;
    }
    if (st.link == LinkState::NoDevice) {
        print_error(cl, "no paired OnePlus Nord Buds found. Pair them in Settings → Bluetooth.");
        finish(cl, 1);
        return;
    }
    if (!st.bt_connected) {
        print_error(cl, "earbuds are not connected. Take them out of the case, or run: nordbuds connect");
        finish(cl, 1);
        return;
    }
    wait_for(cl, [] { return app.buds->ready(); }, std::move(fn), not_ready_message, 20);
}

// Sends a write, then waits for the read-back to confirm it (or for the buds to refuse it).
void confirm(GApplicationCommandLine* cl, std::function<bool()> applied, std::string success) {
    unsigned serial = app.buds->state().error_serial;
    wait_for(
        cl, [applied, serial] { return applied() || app.buds->state().error_serial != serial || !app.buds->ready(); },
        [applied, success](GApplicationCommandLine* c) {
            if (applied()) {
                print(c, success);
                return 0;
            }
            print_error(c, app.buds->ready() ? app.buds->state().last_error : not_ready_message());
            return 1;
        },
        [] { return std::string("the earbuds did not confirm the change"); }, 6);
}

void show_window();

void enter_service() {
    if (app.service) return;
    app.service = true;
    g_application_hold(G_APPLICATION(app.gtk));
    app.indicator = std::make_unique<Indicator>(
        *app.buds, Indicator::Actions{show_window, [] { g_application_quit(G_APPLICATION(app.gtk)); }});
    app.indicator->start();
    if (GDBusConnection* bus = g_application_get_dbus_connection(G_APPLICATION(app.gtk)))
        app.api = std::make_unique<ServiceApi>(bus, *app.buds, show_window);
}

void show_window() {
    enter_service();
    if (!app.window) app.window = std::make_unique<MainWindow>(app.gtk, *app.buds);
    app.window->present();
}

void withdraw_low_battery(int i) {
    app.low_warned[i] = false;
    g_application_withdraw_notification(G_APPLICATION(app.gtk), (std::string("low-battery-") + std::to_string(i)).c_str());
}

void warn_low_battery() {
    if (!app.service) return;
    if (!app.buds->ready()) {
        // A low-battery notice must not outlive the connection it describes.
        for (int i = 0; i < 3; i++)
            if (app.low_warned[i]) withdraw_low_battery(i);
        return;
    }
    const auto& b = app.buds->state().battery;
    const proto::BatteryPart* parts[3] = {&b.left, &b.right, &b.box};
    const char* names[3] = {"Left earbud", "Right earbud", "Charging case"};
    for (int i = 0; i < 3; i++) {
        const auto& p = *parts[i];
        if (p.level < 0) continue;
        if (p.level > 20 || p.charging) {
            if (app.low_warned[i]) withdraw_low_battery(i);
        } else if (p.level <= 15 && !app.low_warned[i]) {
            app.low_warned[i] = true;
            GNotification* n = g_notification_new("Earbuds battery low");
            std::string body = std::string(names[i]) + " is at " + std::to_string(p.level) + "%";
            g_notification_set_body(n, body.c_str());
            g_application_send_notification(G_APPLICATION(app.gtk), (std::string("low-battery-") + std::to_string(i)).c_str(), n);
            g_object_unref(n);
        }
    }
}

void on_buds_changed() {
    if (app.window) app.window->refresh();
    if (app.indicator) app.indicator->update();
    if (app.api) app.api->emit_changed();
    warn_low_battery();
    check_waiters();
}

void ensure_buds(const std::string& device) {
    if (app.buds) return;
    app.buds = std::make_unique<Buds>(device);
    app.buds->add_listener(on_buds_changed);
    Settings::get().add_listener(on_buds_changed);
    app.buds->start();
}

void run_command(GApplicationCommandLine* cl, const std::vector<std::string>& args) {
    const std::string& cmd = args[0];
    auto usage_error = [cl](const std::string& msg) {
        print_error(cl, msg + " (see: nordbuds help)");
        finish(cl, 2);
    };

    if (cmd == "help") {
        print(cl, kUsage);
        finish(cl, 0);
    } else if (cmd == "show") {
        show_window();
        finish(cl, 0);
    } else if (cmd == "quit") {
        print(cl, "Stopping Nord Buds.");
        finish(cl, 0);
        g_application_quit(G_APPLICATION(app.gtk));
    } else if (cmd == "status") {
        with_buds(cl, [](GApplicationCommandLine* c) {
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
        with_buds(cl, [](GApplicationCommandLine* c) {
            print(c, battery_text(app.buds->state()));
            return 0;
        });
    } else if (cmd == "eq") {
        const model::EqPreset* preset = nullptr;
        if (args.size() > 1 && !(preset = model::eq_by_key(args[1])))
            return usage_error("unknown sound profile '" + args[1] + "'");
        with_buds(cl, [preset](GApplicationCommandLine* c) {
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
        with_buds(cl, [want](GApplicationCommandLine* c) {
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
        with_buds(cl, [on](GApplicationCommandLine* c) {
            if (!app.buds->set_find(on)) {
                print_error(c, "these earbuds cannot ring");
                return 1;
            }
            print(c, on ? "Ringing both earbuds. Stop with: nordbuds find off" : "Stopped ringing.");
            return 0;
        });
    } else if (cmd == "touch") {
        if (args.size() == 1) {
            with_buds(cl, [](GApplicationCommandLine* c) {
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
        with_buds(cl, [devs, slot, value](GApplicationCommandLine* c) {
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
    } else if (cmd == "panel") {
        PanelSettings next = Settings::get().panel();
        if (args.size() == 3 && args[1] == "position") {
            if (!Settings::valid_position(args[2])) return usage_error("position must be left, center or right");
            next.position = args[2];
        } else if (args.size() == 3 && args[1] == "order") {
            char* end = nullptr;
            long order = std::strtol(args[2].c_str(), &end, 10);
            if (!end || *end || order < 0 || order > 20) return usage_error("order must be a number from 0 to 20");
            next.order = int(order);
        } else if (args.size() == 3 && args[1] == "label") {
            if (!Settings::valid_label(args[2])) return usage_error("label must be icon, compact, buds or all");
            next.label = args[2];
        } else if (args.size() != 1) {
            return usage_error("panel takes: position <left|center|right>, order <0-20> or label <icon|compact|buds|all>");
        }
        Settings::get().set_panel(next);
        const auto& p = Settings::get().panel();
        print(cl, "Position:  " + p.position + "\nOrder:     " + std::to_string(p.order) + "\nLabel:     " + p.label);
        if (app.service)
            print(cl, Settings::get().extension_active()
                          ? "Shown by:  Nord Buds GNOME extension"
                          : "Shown by:  AppIndicator icon (position follows that extension; log out and back in to "
                            "start the Nord Buds extension)");
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
            cl, [want] { return app.buds->state().bt_connected == want; },
            [want](GApplicationCommandLine* c) {
                print(c, want ? "Connected." : "Disconnected.");
                return 0;
            },
            [] { return "Bluetooth did not respond: " + app.buds->state().last_error; }, 20);
    } else {
        usage_error("unknown command '" + cmd + "'");
    }
}

int on_command_line(GApplication*, GApplicationCommandLine* cl, gpointer) {
    int argc = 0;
    char** argv = g_application_command_line_get_arguments(cl, &argc);
    std::vector<std::string> args;
    std::string device;
    bool background = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--device" && i + 1 < argc) device = argv[++i];
        else if (a == "--background" || a == "-b") background = true;
        else if (a == "--help" || a == "-h") args.insert(args.begin(), "help");
        else args.push_back(a);
    }
    g_strfreev(argv);
    if (device.empty()) {
        const char* env = g_getenv("NORDBUDS_DEVICE");
        if (env) device = env;
    }
    ensure_buds(device);

    if (args.empty()) {
        if (background) enter_service();
        else show_window();
        return 0;
    }
    if (background) enter_service();
    g_object_ref(cl);
    g_application_hold(G_APPLICATION(app.gtk));
    run_command(cl, args);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    app.gtk = gtk_application_new(kAppId, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app.gtk, "command-line", G_CALLBACK(on_command_line), nullptr);
    int status = g_application_run(G_APPLICATION(app.gtk), argc, argv);
    app.window.reset();
    app.api.reset();
    app.indicator.reset();
    app.buds.reset();
    g_object_unref(app.gtk);
    return app.local_exit >= 0 ? app.local_exit : status;
}
