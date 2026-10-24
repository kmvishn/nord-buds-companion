#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>
#include <vector>

class Buds;

// Top-bar icon via StatusNotifierItem + com.canonical.dbusmenu (shown by the AppIndicator extension).
class Indicator {
public:
    struct Actions {
        std::function<void()> open_window;
        std::function<void()> quit;
    };

    Indicator(Buds& buds, Actions actions);
    ~Indicator();
    Indicator(const Indicator&) = delete;
    Indicator& operator=(const Indicator&) = delete;

    void start();
    void update();

    struct Item {
        int id = 0;
        std::string label;
        bool enabled = true;
        bool separator = false;
        const char* toggle_type = nullptr;  // "radio" or "checkmark"
        bool toggled = false;
        std::function<void()> action;
    };

    // Used by the D-Bus vtable callbacks.
    GVariant* sni_property(const char* name);
    GVariant* menu_property(const char* name);
    void sni_method(const char* method, GDBusMethodInvocation* inv);
    void menu_method(const char* method, GVariant* params, GDBusMethodInvocation* inv);

private:
    void build_items();
    GVariant* item_properties(const Item& item);
    GVariant* layout(int parent);
    const Item* find(int id) const;
    void run(int id);
    void try_register();
    std::string label() const;
    std::string tooltip() const;
    std::string status_text() const;

    Buds& buds_;
    Actions actions_;
    std::vector<Item> items_;
    unsigned revision_ = 1;
    std::string bus_name_;
    std::string last_label_;
    std::string last_tooltip_;
    std::string last_title_;
    std::string last_status_ = "Active";
    GDBusConnection* bus_ = nullptr;
    GDBusNodeInfo* node_ = nullptr;
    guint sni_reg_ = 0;
    guint menu_reg_ = 0;
    guint own_id_ = 0;
    guint watch_id_ = 0;
    bool name_owned_ = false;
    bool watcher_present_ = false;
};
