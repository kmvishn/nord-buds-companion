#pragma once

#include <functional>
#include <string>
#include <vector>

struct BudsState;

// Top-bar preferences, stored in ~/.config/nordbuds/settings.ini.
struct PanelSettings {
    std::string position = "right";  // left, center, right
    int order = 0;                   // 0 = first in that area
    std::string label = "compact";   // icon, compact, buds, all
};

class Settings {
public:
    static Settings& get();

    const PanelSettings& panel() const { return panel_; }
    void set_panel(const PanelSettings& panel);

    // True while the Nord Buds GNOME Shell extension is running and showing the top-bar item.
    bool extension_active() const { return extension_active_; }
    void set_extension_active(bool active);

    void add_listener(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

    static bool valid_position(const std::string& p);
    static bool valid_label(const std::string& l);

private:
    Settings();
    void load();
    void save() const;
    void notify();

    PanelSettings panel_;
    bool extension_active_ = false;
    std::vector<std::function<void()>> listeners_;
};

struct LabelStyle {
    const char* key;
    const char* name;
};
const std::vector<LabelStyle>& label_styles();

struct PanelPosition {
    const char* key;
    const char* name;
};
const std::vector<PanelPosition>& panel_positions();

// Top-bar text for the given style; empty when nothing should be shown.
std::string panel_label(const BudsState& st, bool ready, const std::string& style);
