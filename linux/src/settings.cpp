#include "settings.h"

#include <glib.h>

#include <algorithm>

#include "buds.h"

namespace {

std::string settings_path() {
    gchar* p = g_build_filename(g_get_user_config_dir(), "nordbuds", "settings.ini", nullptr);
    std::string s = p;
    g_free(p);
    return s;
}

std::string percent(const proto::BatteryPart& b) {
    return b.level < 0 ? "–" : std::to_string(b.level) + "%";
}

}  // namespace

const std::vector<LabelStyle>& label_styles() {
    static const std::vector<LabelStyle> styles = {
        {"icon", "Icon only"},
        {"compact", "Lowest earbud (e.g. 92%)"},
        {"buds", "Left and right"},
        {"all", "Left, right and case"},
    };
    return styles;
}

const std::vector<PanelPosition>& panel_positions() {
    static const std::vector<PanelPosition> positions = {
        {"left", "Left"},
        {"center", "Center"},
        {"right", "Right"},
    };
    return positions;
}

Settings& Settings::get() {
    static Settings instance;
    return instance;
}

Settings::Settings() {
    load();
}

bool Settings::valid_position(const std::string& p) {
    for (const auto& pos : panel_positions())
        if (p == pos.key) return true;
    return false;
}

bool Settings::valid_label(const std::string& l) {
    for (const auto& s : label_styles())
        if (l == s.key) return true;
    return false;
}

void Settings::load() {
    GKeyFile* kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, settings_path().c_str(), G_KEY_FILE_NONE, nullptr)) {
        gchar* pos = g_key_file_get_string(kf, "panel", "position", nullptr);
        if (pos && valid_position(pos)) panel_.position = pos;
        g_free(pos);
        GError* err = nullptr;
        int order = g_key_file_get_integer(kf, "panel", "order", &err);
        if (!err) panel_.order = std::clamp(order, 0, 20);
        g_clear_error(&err);
        gchar* label = g_key_file_get_string(kf, "panel", "label", nullptr);
        if (label && valid_label(label)) panel_.label = label;
        g_free(label);
    }
    g_key_file_free(kf);
}

void Settings::save() const {
    std::string path = settings_path();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GKeyFile* kf = g_key_file_new();
    g_key_file_set_string(kf, "panel", "position", panel_.position.c_str());
    g_key_file_set_integer(kf, "panel", "order", panel_.order);
    g_key_file_set_string(kf, "panel", "label", panel_.label.c_str());
    GError* err = nullptr;
    if (!g_key_file_save_to_file(kf, path.c_str(), &err)) {
        g_warning("could not save %s: %s", path.c_str(), err->message);
        g_error_free(err);
    }
    g_key_file_free(kf);
}

void Settings::set_panel(const PanelSettings& panel) {
    PanelSettings next = panel;
    next.order = std::clamp(next.order, 0, 20);
    if (!valid_position(next.position)) next.position = panel_.position;
    if (!valid_label(next.label)) next.label = panel_.label;
    if (next.position == panel_.position && next.order == panel_.order && next.label == panel_.label) return;
    panel_ = next;
    save();
    notify();
}

void Settings::set_extension_active(bool active) {
    if (active == extension_active_) return;
    extension_active_ = active;
    notify();
}

void Settings::notify() {
    for (auto& fn : listeners_) fn();
}

std::string panel_label(const BudsState& st, bool ready, const std::string& style) {
    if (!ready || style == "icon") return "";
    const auto& b = st.battery;
    if (style == "compact") {
        int lowest = -1;
        for (const auto* p : {&b.left, &b.right})
            if (p->level >= 0 && (lowest < 0 || p->level < lowest)) lowest = p->level;
        return lowest < 0 ? "" : std::to_string(lowest) + "%";
    }
    std::string s = "L " + percent(b.left) + "  R " + percent(b.right);
    if (style == "all" && b.box.level >= 0) s += "  C " + percent(b.box);
    return s;
}
