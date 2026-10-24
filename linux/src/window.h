#pragma once

#include <gtk/gtk.h>

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "model.h"

class Buds;

class MainWindow {
public:
    MainWindow(GtkApplication* app, Buds& buds);
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    void present();
    void refresh();

private:
    // Ring-shaped battery meter, drawn with cairo and animated towards its target level.
    struct Gauge {
        GtkWidget* area = nullptr;
        GtkWidget* note = nullptr;
        int level = -1;  // -1 = unknown
        bool charging = false;
        double shown = 0;  // level currently drawn, 0..100
        double from = 0;
        gint64 anim_start = 0;
        guint tick = 0;
    };
    struct GestureControl {
        MainWindow* self;
        const model::GestureSlot* slot;
        GtkWidget* dropdown;  // null for slots the firmware fixes to one action
        GtkWidget* fixed;
    };

    void build();
    GtkWidget* build_hero();
    GtkWidget* build_sound();
    GtkWidget* build_touch();
    GtkWidget* build_find();
    GtkWidget* build_menu();
    void apply_panel();
    void set_gauge(Gauge& g, int level, bool charging);

    static void draw_gauge(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data);
    static gboolean gauge_tick(GtkWidget*, GdkFrameClock* clock, gpointer data);
    static void on_eq_toggled(GtkToggleButton* button, gpointer data);
    static void on_game_toggled(GObject* sw, GParamSpec*, gpointer data);
    static void on_side_toggled(GtkToggleButton* button, gpointer data);
    static void on_gesture_selected(GObject* dropdown, GParamSpec*, gpointer data);
    static void on_find_clicked(GtkButton*, gpointer data);
    static void on_connect_clicked(GtkButton*, gpointer data);
    static void on_panel_changed(GObject*, GParamSpec*, gpointer data);
    static void on_panel_order_changed(GtkSpinButton*, gpointer data);

    Buds& buds_;
    GtkWidget* window_ = nullptr;
    GtkWidget* content_ = nullptr;
    GtkWidget* name_ = nullptr;
    GtkWidget* status_dot_ = nullptr;
    GtkWidget* status_ = nullptr;
    GtkWidget* connect_button_ = nullptr;
    GtkWidget* hero_stack_ = nullptr;
    GtkWidget* empty_title_ = nullptr;
    GtkWidget* empty_body_ = nullptr;
    GtkWidget* empty_spinner_ = nullptr;
    GtkWidget* error_ = nullptr;
    std::array<Gauge, 3> gauges_;
    std::vector<std::pair<uint8_t, GtkWidget*>> eq_buttons_;
    GtkWidget* game_switch_ = nullptr;
    GtkWidget* side_left_ = nullptr;
    GtkWidget* side_right_ = nullptr;
    uint8_t touch_dev_ = model::kLeft;
    std::vector<std::unique_ptr<GestureControl>> gestures_;
    GtkWidget* find_button_ = nullptr;
    GtkWidget* find_title_ = nullptr;
    GtkWidget* find_body_ = nullptr;
    GtkWidget* firmware_ = nullptr;
    GtkWidget* address_ = nullptr;
    GtkWidget* panel_position_ = nullptr;
    GtkWidget* panel_order_ = nullptr;
    GtkWidget* panel_label_ = nullptr;
    GtkWidget* panel_note_ = nullptr;
    std::vector<GtkWidget*> device_sections_;
    bool updating_ = false;
};
