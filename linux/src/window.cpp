#include "window.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "buds.h"
#include "settings.h"

namespace {

constexpr const char* kCss = R"CSS(
.nb-window { background-color: @theme_bg_color; }
.nb-header { background: none; box-shadow: none; border: none; }
.nb-app-title { font-weight: 800; letter-spacing: 0.5px; }

.nb-hero {
  border-radius: 24px;
  padding: 20px 20px 18px 20px;
  background-image: linear-gradient(155deg, alpha(@theme_selected_bg_color, 0.20), alpha(@theme_selected_bg_color, 0.03) 70%);
  border: 1px solid alpha(currentColor, 0.07);
}
.nb-name { font-size: 15pt; font-weight: 800; }
.nb-status { font-size: 9.5pt; font-weight: 600; opacity: 0.75; }
.nb-dot { min-width: 8px; min-height: 8px; border-radius: 99px; background-color: alpha(currentColor, 0.35); }
.nb-dot.on { background-color: #2ec27e; box-shadow: 0 0 0 3px alpha(#2ec27e, 0.22); }
.nb-dot.busy { background-color: #e5a50a; animation: nb-blink 1.1s ease-in-out infinite; }
@keyframes nb-blink { 0% { opacity: 1; } 50% { opacity: 0.25; } 100% { opacity: 1; } }

button.nb-pill { border-radius: 99px; padding: 6px 16px; font-weight: 700; }
button.nb-pill.suggested-action { background-image: none; background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; border-color: transparent; box-shadow: 0 2px 8px alpha(@theme_selected_bg_color, 0.35); }

.nb-gauge-name { font-weight: 700; font-size: 10pt; }
.nb-gauge-note { font-size: 8.5pt; opacity: 0.6; min-height: 16px; }
.nb-gauge-note.charging { color: #3584e4; opacity: 1; font-weight: 700; }
.nb-gauge-note.low { color: #e01b24; opacity: 1; font-weight: 700; }

.nb-empty-icon { opacity: 0.4; }
.nb-empty-title { font-size: 13pt; font-weight: 800; }
.nb-empty-body { opacity: 0.65; }
.nb-error { color: #e01b24; background-color: alpha(#e01b24, 0.08); border-radius: 12px; padding: 8px 12px; font-size: 9pt; }

.nb-heading { font-weight: 800; font-size: 10.5pt; opacity: 0.8; margin: 2px 8px 0 8px; }
.nb-card { background-color: alpha(currentColor, 0.045); border-radius: 18px; border: 1px solid alpha(currentColor, 0.05); }
.nb-row { padding: 12px 16px; min-height: 36px; }
.nb-row.nb-sep { border-top: 1px solid alpha(currentColor, 0.07); }
.nb-row-title { font-weight: 600; }
.nb-row-sub { font-size: 9pt; opacity: 0.6; }
.nb-row-icon { min-width: 36px; min-height: 36px; border-radius: 11px; background-color: alpha(@theme_selected_bg_color, 0.14); color: @theme_selected_bg_color; }

button.nb-tile {
  background-image: none; box-shadow: none;
  background-color: alpha(currentColor, 0.05);
  border: 2px solid transparent; border-radius: 16px;
  padding: 10px 6px 10px 6px;
  transition: background-color 180ms ease, border-color 180ms ease;
}
button.nb-tile:hover { background-color: alpha(currentColor, 0.09); }
button.nb-tile:checked { background-color: alpha(@theme_selected_bg_color, 0.15); border-color: @theme_selected_bg_color; color: @theme_fg_color; }
button.nb-tile .nb-curve { color: alpha(currentColor, 0.45); }
button.nb-tile:checked .nb-curve { color: @theme_selected_bg_color; }
.nb-tile-name { font-weight: 700; }
.nb-tile-sub { font-size: 8.5pt; opacity: 0.6; }

.nb-seg { background-color: alpha(currentColor, 0.06); border-radius: 12px; padding: 3px; }
.nb-seg button { background-image: none; background-color: transparent; box-shadow: none; border: none; border-radius: 9px; padding: 5px 0; font-weight: 700; }
.nb-seg button:checked { background-color: @theme_base_color; color: @theme_fg_color; box-shadow: 0 1px 4px alpha(black, 0.18); }

.nb-chip { min-width: 50px; min-height: 30px; border-radius: 10px; background-color: alpha(@theme_selected_bg_color, 0.12); color: @theme_selected_bg_color; font-weight: 800; font-size: 6.5pt; letter-spacing: 0.5px; }
.nb-fixed { opacity: 0.6; font-size: 9.5pt; }
.nb-row dropdown > button { border-radius: 10px; }

button.nb-ring {
  min-width: 52px; min-height: 52px; padding: 0; border-radius: 99px;
  background-image: none; border: none;
  background-color: @theme_selected_bg_color; color: @theme_selected_fg_color;
  box-shadow: 0 3px 10px alpha(@theme_selected_bg_color, 0.35);
}
button.nb-ring.ringing { background-color: #e01b24; animation: nb-pulse 1.3s ease-out infinite; }
@keyframes nb-pulse {
  0% { box-shadow: 0 0 0 0 alpha(#e01b24, 0.55); }
  100% { box-shadow: 0 0 0 16px alpha(#e01b24, 0); }
}

.nb-pop-heading { font-weight: 800; }
.nb-dim { opacity: 0.6; font-size: 9pt; }
)CSS";

struct Rgb {
    double r, g, b;
};

Rgb hex(unsigned v) {
    return {((v >> 16) & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0, (v & 0xFF) / 255.0};
}

// Gradient ends for a battery arc: blue while charging, then green, amber, red by level.
std::pair<Rgb, Rgb> level_colors(int level, bool charging) {
    if (charging) return {hex(0x1c71d8), hex(0x62a0ea)};
    if (level >= 50) return {hex(0x26a269), hex(0x57e389)};
    if (level >= 20) return {hex(0xe5a50a), hex(0xf6d32d)};
    return {hex(0xc01c28), hex(0xf66151)};
}

// Illustrative response shapes for the EQ tiles (y: 0 bottom .. 1 top, low to high frequencies).
const double* eq_curve(int id) {
    static const double balanced[5] = {0.48, 0.54, 0.50, 0.54, 0.48};
    static const double bold[5] = {0.80, 0.46, 0.30, 0.62, 0.88};
    static const double bass[5] = {0.95, 0.80, 0.42, 0.30, 0.30};
    if (id == 1) return bold;
    if (id == 2) return bass;
    return balanced;
}

const char* eq_blurb(int id) {
    if (id == 1) return "Punchy";
    if (id == 2) return "Deeper lows";
    return "Natural";
}

const char* gesture_glyph(uint8_t act) {
    switch (act) {
    case 0x01: return "●";
    case 0x02: return "● ●";
    case 0x03: return "● ● ●";
    default: return "HOLD";
    }
}

GtkWidget* label(const std::string& text, const char* css = nullptr, float xalign = 0.0f) {
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    if (css) gtk_widget_add_css_class(l, css);
    return l;
}

GtkWidget* vbox(int spacing) {
    return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
}

GtkWidget* hbox(int spacing) {
    return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
}

// A titled section: heading above a rounded card that holds rows.
GtkWidget* section(const char* title, GtkWidget** card_out) {
    GtkWidget* box = vbox(8);
    gtk_box_append(GTK_BOX(box), label(title, "nb-heading"));
    GtkWidget* card = vbox(0);
    gtk_widget_add_css_class(card, "nb-card");
    gtk_box_append(GTK_BOX(box), card);
    *card_out = card;
    return box;
}

// One row in a card: optional icon, title + subtitle, and a control on the right.
GtkWidget* row(GtkWidget* card, const char* icon, GtkWidget* title_box, GtkWidget* suffix) {
    GtkWidget* r = hbox(12);
    gtk_widget_add_css_class(r, "nb-row");
    if (gtk_widget_get_first_child(card)) gtk_widget_add_css_class(r, "nb-sep");
    if (icon) {
        GtkWidget* img = gtk_image_new_from_icon_name(icon);
        gtk_image_set_pixel_size(GTK_IMAGE(img), 18);
        gtk_widget_add_css_class(img, "nb-row-icon");
        gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(r), img);
    }
    gtk_widget_set_hexpand(title_box, TRUE);
    gtk_widget_set_valign(title_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(r), title_box);
    if (suffix) {
        gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(r), suffix);
    }
    gtk_box_append(GTK_BOX(card), r);
    return r;
}

GtkWidget* titles(const char* title, const char* subtitle, GtkWidget** title_out = nullptr,
                  GtkWidget** sub_out = nullptr) {
    GtkWidget* box = vbox(2);
    GtkWidget* t = label(title, "nb-row-title");
    gtk_box_append(GTK_BOX(box), t);
    GtkWidget* s = label(subtitle ? subtitle : "", "nb-row-sub");
    gtk_widget_set_visible(s, subtitle != nullptr);
    gtk_box_append(GTK_BOX(box), s);
    if (title_out) *title_out = t;
    if (sub_out) *sub_out = s;
    return box;
}

void load_css() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, kCss);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), NORDBUDS_ICON_ROOT);
}

const char* app_icon() {
    static const bool custom =
        g_file_test(NORDBUDS_ICON_ROOT "/hicolor/scalable/apps/nordbuds-symbolic.svg", G_FILE_TEST_EXISTS);
    return custom ? "nordbuds-symbolic" : "audio-headphones-symbolic";
}

void draw_eq_curve(GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer data) {
    const double* y = eq_curve(GPOINTER_TO_INT(data));
    GdkRGBA color;
    gtk_widget_get_color(GTK_WIDGET(area), &color);
    const double pad = 4;
    auto px = [&](int i) { return pad + (w - 2 * pad) * i / 4.0; };
    auto py = [&](int i) { return pad + (h - 2 * pad) * (1.0 - y[i]); };

    // Catmull-Rom through the five points, as cubic beziers.
    auto path = [&] {
        cairo_move_to(cr, px(0), py(0));
        for (int i = 0; i < 4; i++) {
            int i0 = std::max(i - 1, 0), i3 = std::min(i + 2, 4);
            cairo_curve_to(cr, px(i) + (px(i + 1) - px(i0)) / 6, py(i) + (py(i + 1) - py(i0)) / 6,
                           px(i + 1) - (px(i3) - px(i)) / 6, py(i + 1) - (py(i3) - py(i)) / 6, px(i + 1), py(i + 1));
        }
    };
    path();
    cairo_line_to(cr, px(4), h);
    cairo_line_to(cr, px(0), h);
    cairo_close_path(cr);
    cairo_pattern_t* fill = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgba(fill, 0, color.red, color.green, color.blue, color.alpha * 0.30);
    cairo_pattern_add_color_stop_rgba(fill, 1, color.red, color.green, color.blue, 0);
    cairo_set_source(cr, fill);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);

    path();
    cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
}

std::string status_text(const Buds& buds) {
    const auto& st = buds.state();
    if (!st.bluetooth) return "Bluetooth is off";
    switch (st.link) {
    case LinkState::NoDevice: return "No earbuds paired";
    case LinkState::Disconnected: return "Not connected";
    case LinkState::Connecting: return st.bt_connected ? "Connecting…" : "Not connected";
    case LinkState::Connected: return buds.ready() ? "Connected" : "Reading settings…";
    }
    return "";
}

}  // namespace

MainWindow::MainWindow(GtkApplication* app, Buds& buds) : buds_(buds) {
    load_css();
    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Nord Buds");
    gtk_window_set_icon_name(GTK_WINDOW(window_), "audio-headphones");
    gtk_window_set_default_size(GTK_WINDOW(window_), 440, 860);
    gtk_window_set_hide_on_close(GTK_WINDOW(window_), TRUE);
    gtk_widget_add_css_class(window_, "nb-window");
    build();
    refresh();
}

void MainWindow::present() {
    gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::build() {
    GtkWidget* header = gtk_header_bar_new();
    gtk_widget_add_css_class(header, "nb-header");
    GtkWidget* app_title = label("Nord Buds", "nb-app-title", 0.5f);
    gtk_label_set_wrap(GTK_LABEL(app_title), FALSE);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), app_title);
    GtkWidget* menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(menu, "Top bar and device info");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), build_menu());
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu);
    gtk_window_set_titlebar(GTK_WINDOW(window_), header);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    content_ = vbox(22);
    GtkWidget* content = content_;
    gtk_widget_set_margin_top(content, 6);
    gtk_widget_set_margin_bottom(content, 24);
    gtk_widget_set_margin_start(content, 18);
    gtk_widget_set_margin_end(content, 18);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), content);
    gtk_window_set_child(GTK_WINDOW(window_), scroller);

    gtk_box_append(GTK_BOX(content), build_hero());
    for (GtkWidget* s : {build_sound(), build_touch(), build_find()}) {
        gtk_box_append(GTK_BOX(content), s);
        device_sections_.push_back(s);
    }
}

GtkWidget* MainWindow::build_hero() {
    GtkWidget* hero = vbox(16);
    gtk_widget_add_css_class(hero, "nb-hero");

    GtkWidget* top = hbox(12);
    GtkWidget* ident = vbox(4);
    gtk_widget_set_hexpand(ident, TRUE);
    name_ = label("OnePlus Nord Buds 2r", "nb-name");
    gtk_label_set_ellipsize(GTK_LABEL(name_), PANGO_ELLIPSIZE_END);
    gtk_label_set_wrap(GTK_LABEL(name_), FALSE);
    gtk_box_append(GTK_BOX(ident), name_);
    GtkWidget* status_row = hbox(8);
    status_dot_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(status_dot_, "nb-dot");
    gtk_widget_set_valign(status_dot_, GTK_ALIGN_CENTER);
    status_ = label("", "nb-status");
    gtk_box_append(GTK_BOX(status_row), status_dot_);
    gtk_box_append(GTK_BOX(status_row), status_);
    gtk_box_append(GTK_BOX(ident), status_row);
    connect_button_ = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(connect_button_, "nb-pill");
    gtk_widget_set_valign(connect_button_, GTK_ALIGN_CENTER);
    g_signal_connect(connect_button_, "clicked", G_CALLBACK(on_connect_clicked), this);
    gtk_box_append(GTK_BOX(top), ident);
    gtk_box_append(GTK_BOX(top), connect_button_);
    gtk_box_append(GTK_BOX(hero), top);

    hero_stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(hero_stack_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_vhomogeneous(GTK_STACK(hero_stack_), FALSE);
    gtk_stack_set_interpolate_size(GTK_STACK(hero_stack_), TRUE);

    GtkWidget* meters = hbox(8);
    gtk_box_set_homogeneous(GTK_BOX(meters), TRUE);
    const char* names[3] = {"Left", "Right", "Case"};
    for (int i = 0; i < 3; i++) {
        Gauge& g = gauges_[size_t(i)];
        GtkWidget* col = vbox(2);
        g.area = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g.area), 104);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g.area), 104);
        gtk_widget_set_halign(g.area, GTK_ALIGN_CENTER);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g.area), draw_gauge, &g, nullptr);
        gtk_box_append(GTK_BOX(col), g.area);
        GtkWidget* name = label(names[i], "nb-gauge-name", 0.5f);
        gtk_widget_set_margin_top(name, 6);
        gtk_box_append(GTK_BOX(col), name);
        g.note = label("", "nb-gauge-note", 0.5f);
        gtk_box_append(GTK_BOX(col), g.note);
        gtk_box_append(GTK_BOX(meters), col);
    }
    gtk_stack_add_named(GTK_STACK(hero_stack_), meters, "meters");

    GtkWidget* empty = vbox(6);
    gtk_widget_set_margin_top(empty, 8);
    gtk_widget_set_margin_bottom(empty, 8);
    GtkWidget* icon = gtk_image_new_from_icon_name(app_icon());
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 72);
    gtk_widget_add_css_class(icon, "nb-empty-icon");
    gtk_widget_set_margin_bottom(icon, 6);
    gtk_box_append(GTK_BOX(empty), icon);
    GtkWidget* title_row = hbox(8);
    gtk_widget_set_halign(title_row, GTK_ALIGN_CENTER);
    empty_spinner_ = gtk_spinner_new();
    empty_title_ = label("", "nb-empty-title", 0.5f);
    gtk_box_append(GTK_BOX(title_row), empty_spinner_);
    gtk_box_append(GTK_BOX(title_row), empty_title_);
    gtk_box_append(GTK_BOX(empty), title_row);
    empty_body_ = label("", "nb-empty-body", 0.5f);
    gtk_label_set_justify(GTK_LABEL(empty_body_), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(empty), empty_body_);
    gtk_stack_add_named(GTK_STACK(hero_stack_), empty, "empty");
    gtk_box_append(GTK_BOX(hero), hero_stack_);

    error_ = label("", "nb-error");
    gtk_box_append(GTK_BOX(hero), error_);
    return hero;
}

GtkWidget* MainWindow::build_sound() {
    GtkWidget* card = nullptr;
    GtkWidget* box = section("Sound", &card);

    GtkWidget* tiles = hbox(10);
    gtk_box_set_homogeneous(GTK_BOX(tiles), TRUE);
    gtk_widget_set_margin_start(tiles, 14);
    gtk_widget_set_margin_end(tiles, 14);
    gtk_widget_set_margin_top(tiles, 14);
    gtk_widget_set_margin_bottom(tiles, 14);
    GtkWidget* first = nullptr;
    for (const auto& preset : model::eq_presets()) {
        GtkWidget* b = gtk_toggle_button_new();
        gtk_widget_add_css_class(b, "nb-tile");
        GtkWidget* inner = vbox(2);
        GtkWidget* curve = gtk_drawing_area_new();
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(curve), 40);
        gtk_widget_add_css_class(curve, "nb-curve");
        gtk_widget_set_margin_bottom(curve, 6);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(curve), draw_eq_curve, GINT_TO_POINTER(preset.id), nullptr);
        gtk_box_append(GTK_BOX(inner), curve);
        gtk_box_append(GTK_BOX(inner), label(preset.name, "nb-tile-name", 0.5f));
        gtk_box_append(GTK_BOX(inner), label(eq_blurb(preset.id), "nb-tile-sub", 0.5f));
        gtk_button_set_child(GTK_BUTTON(b), inner);
        if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(first));
        else first = b;
        g_object_set_data(G_OBJECT(b), "eq-id", GINT_TO_POINTER(preset.id));
        g_signal_connect(b, "toggled", G_CALLBACK(on_eq_toggled), this);
        gtk_box_append(GTK_BOX(tiles), b);
        eq_buttons_.push_back({preset.id, b});
    }
    gtk_box_append(GTK_BOX(card), tiles);

    game_switch_ = gtk_switch_new();
    g_signal_connect(game_switch_, "notify::active", G_CALLBACK(on_game_toggled), this);
    row(card, "input-gaming-symbolic", titles("Game mode", "Lower latency for games and video"), game_switch_);
    return box;
}

GtkWidget* MainWindow::build_touch() {
    GtkWidget* card = nullptr;
    GtkWidget* box = section("Touch controls", &card);

    GtkWidget* seg = hbox(0);
    gtk_widget_add_css_class(seg, "nb-seg");
    gtk_box_set_homogeneous(GTK_BOX(seg), TRUE);
    gtk_widget_set_margin_start(seg, 14);
    gtk_widget_set_margin_end(seg, 14);
    gtk_widget_set_margin_top(seg, 14);
    gtk_widget_set_margin_bottom(seg, 4);
    side_left_ = gtk_toggle_button_new_with_label("Left earbud");
    side_right_ = gtk_toggle_button_new_with_label("Right earbud");
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(side_right_), GTK_TOGGLE_BUTTON(side_left_));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(side_left_), TRUE);
    for (GtkWidget* b : {side_left_, side_right_}) {
        g_signal_connect(b, "toggled", G_CALLBACK(on_side_toggled), this);
        gtk_box_append(GTK_BOX(seg), b);
    }
    gtk_box_append(GTK_BOX(card), seg);

    GtkWidget* rows = vbox(0);
    for (const auto& slot : model::gesture_slots()) {
        GtkWidget* chip = label(gesture_glyph(slot.act), "nb-chip", 0.5f);
        gtk_widget_set_valign(chip, GTK_ALIGN_CENTER);
        GtkWidget* r = hbox(12);
        gtk_widget_add_css_class(r, "nb-row");
        gtk_box_append(GTK_BOX(r), chip);
        GtkWidget* name = label(slot.name, "nb-row-title");
        gtk_widget_set_hexpand(name, TRUE);
        gtk_box_append(GTK_BOX(r), name);

        auto ctl = std::make_unique<GestureControl>(GestureControl{this, &slot, nullptr, nullptr});
        if (slot.options.size() > 1) {
            std::vector<std::string> names;
            for (uint8_t fn : slot.options) names.push_back(model::function_name(fn));
            std::vector<const char*> c_names;
            for (const auto& n : names) c_names.push_back(n.c_str());
            c_names.push_back(nullptr);
            ctl->dropdown = gtk_drop_down_new_from_strings(c_names.data());
            gtk_widget_set_size_request(ctl->dropdown, 168, -1);
            gtk_widget_set_valign(ctl->dropdown, GTK_ALIGN_CENTER);
            g_signal_connect(ctl->dropdown, "notify::selected", G_CALLBACK(on_gesture_selected), ctl.get());
            gtk_box_append(GTK_BOX(r), ctl->dropdown);
        } else {
            ctl->fixed = label(model::function_name(slot.options.front()), "nb-fixed", 1.0f);
            gtk_label_set_wrap(GTK_LABEL(ctl->fixed), FALSE);
            gtk_widget_set_tooltip_text(ctl->fixed, "This firmware only allows this action here");
            gtk_box_append(GTK_BOX(r), ctl->fixed);
        }
        gtk_box_append(GTK_BOX(rows), r);
        gestures_.push_back(std::move(ctl));
    }
    gtk_box_append(GTK_BOX(card), rows);
    return box;
}

GtkWidget* MainWindow::build_find() {
    GtkWidget* card = nullptr;
    GtkWidget* box = section("Find my earbuds", &card);
    find_button_ = gtk_button_new_from_icon_name("audio-volume-high-symbolic");
    gtk_widget_add_css_class(find_button_, "nb-ring");
    gtk_widget_set_tooltip_text(find_button_, "Ring the earbuds");
    g_signal_connect(find_button_, "clicked", G_CALLBACK(on_find_clicked), this);
    GtkWidget* text = titles("Ring earbuds", "Plays a loud tone on both earbuds. Take them out of your ears first.",
                             &find_title_, &find_body_);
    GtkWidget* r = row(card, nullptr, text, nullptr);
    gtk_box_prepend(GTK_BOX(r), find_button_);
    gtk_widget_set_valign(find_button_, GTK_ALIGN_CENTER);
    gtk_box_set_spacing(GTK_BOX(r), 16);
    gtk_widget_set_margin_top(r, 4);
    gtk_widget_set_margin_bottom(r, 4);
    return box;
}

GtkWidget* MainWindow::build_menu() {
    GtkWidget* pop = gtk_popover_new();
    GtkWidget* box = vbox(10);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_size_request(box, 320, -1);

    gtk_box_append(GTK_BOX(box), label("Top bar", "nb-pop-heading"));
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);

    std::vector<const char*> positions;
    for (const auto& pos : panel_positions()) positions.push_back(pos.name);
    positions.push_back(nullptr);
    panel_position_ = gtk_drop_down_new_from_strings(positions.data());
    g_signal_connect(panel_position_, "notify::selected", G_CALLBACK(on_panel_changed), this);

    panel_order_ = gtk_spin_button_new_with_range(0, 20, 1);
    g_signal_connect(panel_order_, "value-changed", G_CALLBACK(on_panel_order_changed), this);

    std::vector<const char*> labels;
    for (const auto& style : label_styles()) labels.push_back(style.name);
    labels.push_back(nullptr);
    panel_label_ = gtk_drop_down_new_from_strings(labels.data());
    g_signal_connect(panel_label_, "notify::selected", G_CALLBACK(on_panel_changed), this);

    const char* names[] = {"Position", "Order", "Show"};
    GtkWidget* controls[] = {panel_position_, panel_order_, panel_label_};
    for (int i = 0; i < 3; i++) {
        GtkWidget* name = label(names[i]);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_widget_set_size_request(controls[i], 190, -1);
        gtk_widget_set_halign(controls[i], GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), name, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), controls[i], 1, i, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    panel_note_ = label("", "nb-dim");
    gtk_label_set_max_width_chars(GTK_LABEL(panel_note_), 40);
    gtk_box_append(GTK_BOX(box), panel_note_);

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), label("These earbuds", "nb-pop-heading"));
    firmware_ = label("", "nb-dim");
    address_ = label("", "nb-dim");
    gtk_label_set_selectable(GTK_LABEL(address_), TRUE);
    gtk_box_append(GTK_BOX(box), firmware_);
    gtk_box_append(GTK_BOX(box), address_);

    gtk_popover_set_child(GTK_POPOVER(pop), box);
    return pop;
}

void MainWindow::on_panel_changed(GObject*, GParamSpec*, gpointer data) {
    static_cast<MainWindow*>(data)->apply_panel();
}

void MainWindow::on_panel_order_changed(GtkSpinButton*, gpointer data) {
    static_cast<MainWindow*>(data)->apply_panel();
}

void MainWindow::apply_panel() {
    if (updating_) return;
    PanelSettings next = Settings::get().panel();
    guint pos = gtk_drop_down_get_selected(GTK_DROP_DOWN(panel_position_));
    if (pos < panel_positions().size()) next.position = panel_positions()[pos].key;
    next.order = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(panel_order_));
    guint style = gtk_drop_down_get_selected(GTK_DROP_DOWN(panel_label_));
    if (style < label_styles().size()) next.label = label_styles()[style].key;
    Settings::get().set_panel(next);
}

void MainWindow::set_gauge(Gauge& g, int level, bool charging) {
    if (g.level == level && g.charging == charging) return;
    g.level = level;
    g.charging = charging;
    g.from = g.shown;
    g.anim_start = 0;
    if (!g.tick) g.tick = gtk_widget_add_tick_callback(g.area, gauge_tick, &g, nullptr);
    gtk_widget_queue_draw(g.area);
}

gboolean MainWindow::gauge_tick(GtkWidget* area, GdkFrameClock* clock, gpointer data) {
    auto* g = static_cast<Gauge*>(data);
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    if (!g->anim_start) g->anim_start = now;
    double t = std::min(1.0, double(now - g->anim_start) / (700.0 * 1000));
    double ease = 1 - std::pow(1 - t, 3);
    double target = std::max(g->level, 0);
    g->shown = g->from + (target - g->from) * ease;
    gtk_widget_queue_draw(area);
    if (t < 1) return G_SOURCE_CONTINUE;
    g->tick = 0;
    return G_SOURCE_REMOVE;
}

void MainWindow::draw_gauge(GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer data) {
    auto* g = static_cast<Gauge*>(data);
    GdkRGBA fg;
    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    const double line = 9;
    const double cx = w / 2.0, cy = h / 2.0;
    const double radius = std::min(w, h) / 2.0 - line;
    const double start = -M_PI / 2;

    cairo_set_line_width(cr, line);
    cairo_arc(cr, cx, cy, radius, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, g->level < 0 ? 0.08 : 0.11);
    cairo_stroke(cr);

    if (g->level >= 0 && g->shown > 0.2) {
        auto [c1, c2] = level_colors(g->level, g->charging);
        double frac = std::min(g->shown, 100.0) / 100.0;
        double end = start + frac * 2 * M_PI;
        // Soft glow under the arc.
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, line + 7);
        cairo_set_source_rgba(cr, c2.r, c2.g, c2.b, 0.16);
        cairo_arc(cr, cx, cy, radius, start, end);
        cairo_stroke(cr);
        // The arc itself, shaded from c1 to c2 along its length.
        int steps = std::max(1, int(frac * 120));
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        cairo_set_line_width(cr, line);
        for (int i = 0; i < steps; i++) {
            double a0 = start + (end - start) * i / steps;
            double a1 = start + (end - start) * (i + 1) / steps + 0.01;
            double k = double(i) / steps;
            cairo_set_source_rgb(cr, c1.r + (c2.r - c1.r) * k, c1.g + (c2.g - c1.g) * k, c1.b + (c2.b - c1.b) * k);
            cairo_arc(cr, cx, cy, radius, a0, std::min(a1, end));
            cairo_stroke(cr);
        }
        for (auto [a, c] : {std::pair{start, c1}, std::pair{end, c2}}) {
            cairo_set_source_rgb(cr, c.r, c.g, c.b);
            cairo_arc(cr, cx + radius * std::cos(a), cy + radius * std::sin(a), line / 2, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    // Number and "%" as separate layouts with explicit colours: Pango's alpha attribute leaks into
    // later text when GTK renders with cairo.
    auto layout = [area](const char* markup) {
        PangoLayout* l = gtk_widget_create_pango_layout(GTK_WIDGET(area), nullptr);
        pango_layout_set_markup(l, markup, -1);
        return l;
    };
    std::string number = g->level < 0 ? "–" : std::to_string(int(std::lround(g->shown)));
    PangoLayout* big = layout(("<span size='19456' weight='800'>" + number + "</span>").c_str());
    PangoLayout* unit = g->level < 0 ? nullptr : layout("<span size='10240' weight='700'>%</span>");
    int bw = 0, bh = 0, uw = 0, uh = 0;
    pango_layout_get_pixel_size(big, &bw, &bh);
    if (unit) pango_layout_get_pixel_size(unit, &uw, &uh);
    double x = cx - (bw + uw) / 2.0, y = cy - bh / 2.0;
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, g->level < 0 ? fg.alpha * 0.45 : fg.alpha);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, big);
    if (unit) {
        // Sit the "%" on the number's baseline.
        double baseline = pango_layout_get_baseline(big) / double(PANGO_SCALE);
        double unit_baseline = pango_layout_get_baseline(unit) / double(PANGO_SCALE);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha * 0.6);
        cairo_move_to(cr, x + bw, y + baseline - unit_baseline);
        pango_cairo_show_layout(cr, unit);
        g_object_unref(unit);
    }
    g_object_unref(big);
}

void MainWindow::refresh() {
    updating_ = true;
    const auto& st = buds_.state();
    bool ready = buds_.ready();
    bool busy = st.bt_connected && !ready;

    gtk_label_set_text(GTK_LABEL(name_), st.name.empty() ? "OnePlus Nord Buds 2r" : st.name.c_str());
    gtk_label_set_text(GTK_LABEL(status_), status_text(buds_).c_str());
    gtk_widget_remove_css_class(status_dot_, "on");
    gtk_widget_remove_css_class(status_dot_, "busy");
    if (ready) gtk_widget_add_css_class(status_dot_, "on");
    else if (busy) gtk_widget_add_css_class(status_dot_, "busy");

    gtk_widget_set_visible(connect_button_, st.link != LinkState::NoDevice);
    gtk_button_set_label(GTK_BUTTON(connect_button_), st.bt_connected ? "Disconnect" : "Connect");
    if (st.bt_connected) gtk_widget_remove_css_class(connect_button_, "suggested-action");
    else gtk_widget_add_css_class(connect_button_, "suggested-action");

    gtk_stack_set_visible_child_name(GTK_STACK(hero_stack_), ready ? "meters" : "empty");
    if (!st.bluetooth) {
        gtk_label_set_text(GTK_LABEL(empty_title_), "Bluetooth is off");
        gtk_label_set_text(GTK_LABEL(empty_body_), "Nord Buds will connect by itself\nonce Bluetooth is back on.");
    } else if (st.link == LinkState::NoDevice) {
        gtk_label_set_text(GTK_LABEL(empty_title_), "No earbuds paired");
        gtk_label_set_text(GTK_LABEL(empty_body_), "Pair your OnePlus Nord Buds in\nSettings → Bluetooth first.");
    } else if (busy) {
        gtk_label_set_text(GTK_LABEL(empty_title_), "Connecting…");
        gtk_label_set_text(GTK_LABEL(empty_body_), "Opening the control channel to your earbuds.");
    } else {
        gtk_label_set_text(GTK_LABEL(empty_title_), "Not connected");
        gtk_label_set_text(GTK_LABEL(empty_body_), "Take the earbuds out of the case,\nor press Connect.");
    }
    gtk_widget_set_visible(empty_spinner_, busy);
    gtk_spinner_set_spinning(GTK_SPINNER(empty_spinner_), busy);

    bool show_error = !st.last_error.empty() && !ready && st.bt_connected;
    gtk_label_set_text(GTK_LABEL(error_), show_error ? st.last_error.c_str() : "");
    gtk_widget_set_visible(error_, show_error);

    // Battery is only meaningful while the control channel is up; otherwise it reads as unknown.
    const proto::BatteryPart* parts[3] = {&st.battery.left, &st.battery.right, &st.battery.box};
    for (size_t i = 0; i < 3; i++) {
        const auto& b = *parts[i];
        int level = ready ? b.level : -1;
        bool charging = ready && b.charging && level >= 0;
        set_gauge(gauges_[i], level, charging);
        bool low = !charging && level >= 0 && level < 20;
        const char* note = "";
        if (charging) note = "Charging";
        else if (low) note = "Low";
        else if (ready && level < 0) note = i == 2 ? "Open the lid" : "Not reported";
        gtk_label_set_text(GTK_LABEL(gauges_[i].note), note);
        if (charging) gtk_widget_add_css_class(gauges_[i].note, "charging");
        else gtk_widget_remove_css_class(gauges_[i].note, "charging");
        if (low) gtk_widget_add_css_class(gauges_[i].note, "low");
        else gtk_widget_remove_css_class(gauges_[i].note, "low");
    }

    for (auto& [id, button] : eq_buttons_) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), st.eq == id);
        gtk_widget_set_sensitive(button, buds_.supports(proto::kEqSet));
    }
    gtk_switch_set_active(GTK_SWITCH(game_switch_), st.game_mode == 1);
    gtk_widget_set_sensitive(game_switch_, buds_.supports(proto::kFeatureSet) && st.game_mode >= 0);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(touch_dev_ == model::kLeft ? side_left_ : side_right_), TRUE);
    bool can_touch = !st.gestures.empty() && buds_.supports(proto::kGesturesSet);
    for (auto& ctl : gestures_) {
        if (!ctl->dropdown) continue;
        guint selected = GTK_INVALID_LIST_POSITION;
        for (const auto& e : st.gestures) {
            if (e.dev != touch_dev_ || e.act != ctl->slot->act) continue;
            for (size_t i = 0; i < ctl->slot->options.size(); i++)
                if (ctl->slot->options[i] == e.fn) selected = guint(i);
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(ctl->dropdown), selected);
        gtk_widget_set_sensitive(ctl->dropdown, can_touch);
    }

    gtk_label_set_text(GTK_LABEL(find_title_), st.finding ? "Ringing…" : "Ring earbuds");
    gtk_label_set_text(GTK_LABEL(find_body_), st.finding
                                                  ? "Press again to stop. Stops by itself after a minute."
                                                  : "Plays a loud tone on both earbuds. Take them out of your ears first.");
    gtk_button_set_icon_name(GTK_BUTTON(find_button_),
                             st.finding ? "media-playback-stop-symbolic" : "audio-volume-high-symbolic");
    if (st.finding) gtk_widget_add_css_class(find_button_, "ringing");
    else gtk_widget_remove_css_class(find_button_, "ringing");
    gtk_widget_set_sensitive(find_button_, buds_.supports(proto::kFind));

    for (GtkWidget* s : device_sections_) gtk_widget_set_visible(s, ready);
    // With only the status card to show, keep it centred instead of stranded at the top.
    gtk_widget_set_valign(content_, ready ? GTK_ALIGN_START : GTK_ALIGN_CENTER);

    std::string fw = "Firmware ";
    if (!st.firmware.summary.empty()) {
        fw += st.firmware.summary;
        if (!st.firmware.left.empty())
            fw += "\nLeft " + st.firmware.left + " · Right " + st.firmware.right + " · Case " + st.firmware.box;
    } else {
        fw += "–";
    }
    gtk_label_set_text(GTK_LABEL(firmware_), fw.c_str());
    gtk_label_set_text(GTK_LABEL(address_), ("Bluetooth " + (st.address.empty() ? std::string("–") : st.address)).c_str());

    const auto& panel = Settings::get().panel();
    for (size_t i = 0; i < panel_positions().size(); i++)
        if (panel.position == panel_positions()[i].key) gtk_drop_down_set_selected(GTK_DROP_DOWN(panel_position_), guint(i));
    for (size_t i = 0; i < label_styles().size(); i++)
        if (panel.label == label_styles()[i].key) gtk_drop_down_set_selected(GTK_DROP_DOWN(panel_label_), guint(i));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(panel_order_), panel.order);
    bool extension = Settings::get().extension_active();
    gtk_widget_set_sensitive(panel_position_, extension);
    gtk_widget_set_sensitive(panel_order_, extension);
    gtk_label_set_text(GTK_LABEL(panel_note_),
                       extension ? "Shown by the Nord Buds GNOME extension."
                                 : "Shown by the AppIndicator extension, which sets its own position. "
                                   "Log out and back in once to start the Nord Buds extension.");
    updating_ = false;
}

void MainWindow::on_eq_toggled(GtkToggleButton* button, gpointer data) {
    auto* self = static_cast<MainWindow*>(data);
    if (self->updating_ || !gtk_toggle_button_get_active(button)) return;
    int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "eq-id"));
    if (id == self->buds_.state().eq) return;
    if (!self->buds_.set_eq(id)) self->refresh();
}

void MainWindow::on_game_toggled(GObject* sw, GParamSpec*, gpointer data) {
    auto* self = static_cast<MainWindow*>(data);
    if (self->updating_) return;
    if (!self->buds_.set_game_mode(gtk_switch_get_active(GTK_SWITCH(sw)))) self->refresh();
}

void MainWindow::on_side_toggled(GtkToggleButton* button, gpointer data) {
    auto* self = static_cast<MainWindow*>(data);
    if (self->updating_ || !gtk_toggle_button_get_active(button)) return;
    self->touch_dev_ = GTK_WIDGET(button) == self->side_left_ ? model::kLeft : model::kRight;
    self->refresh();
}

void MainWindow::on_gesture_selected(GObject* dropdown, GParamSpec*, gpointer data) {
    auto* ctl = static_cast<GestureControl*>(data);
    if (ctl->self->updating_) return;
    guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (index >= ctl->slot->options.size()) return;
    if (!ctl->self->buds_.set_gesture({ctl->self->touch_dev_}, ctl->slot->act, ctl->slot->options[index]))
        ctl->self->refresh();
}

void MainWindow::on_find_clicked(GtkButton*, gpointer data) {
    auto* self = static_cast<MainWindow*>(data);
    self->buds_.set_find(!self->buds_.state().finding);
}

void MainWindow::on_connect_clicked(GtkButton*, gpointer data) {
    auto* self = static_cast<MainWindow*>(data);
    if (self->buds_.state().bt_connected) self->buds_.bluetooth_disconnect();
    else self->buds_.bluetooth_connect();
}
