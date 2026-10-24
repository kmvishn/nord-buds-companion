#pragma once

#include <windows.h>
#include <d2d1.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "gfx.h"
#include "model.h"

class Buds;

// The main window, drawn with Direct2D: battery rings, sound, touch controls and find.
class MainWindow {
public:
    struct Actions {
        std::function<void()> quit;
        std::function<void()> bluetooth_settings;
    };

    MainWindow(Buds& buds, Actions actions);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    void present();
    void refresh();

    // Draws the whole window into rt, in DIPs. Also used for offscreen snapshots.
    void paint(ID2D1RenderTarget* rt, float width, float height);
    float content_height(float width) const;

private:
    enum Hit : int { kNone = 0, kConnect, kMore, kGame, kSideLeft, kSideRight, kFind, kEq = 100, kGesture = 200 };

    // Ring-shaped battery meter, animated towards its target level.
    struct Gauge {
        int level = -1;  // -1 = unknown
        bool charging = false;
        float shown = 0;  // level currently drawn, 0..100
        float from = 0;
        ULONGLONG start = 0;
        bool animating = false;
    };
    struct Box {
        D2D1_RECT_F rect;
        int id;
    };
    struct Layout {
        float height = 0;
        bool ready = false;
        std::wstring connect_label;
        D2D1_RECT_F hero{}, name{}, status{}, connect{}, more{}, body{}, error{};
        std::array<D2D1_RECT_F, 3> rings{};
        D2D1_RECT_F sound_head{}, sound_card{}, game_row{}, game_switch{};
        std::array<D2D1_RECT_F, 3> tiles{};
        D2D1_RECT_F touch_head{}, touch_card{}, seg{};
        std::array<D2D1_RECT_F, 2> sides{};
        std::vector<D2D1_RECT_F> gesture_rows, gesture_controls;
        D2D1_RECT_F find_head{}, find_card{}, find_button{}, find_title{}, find_body{};
    };
    struct Painter;

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT on_message(UINT msg, WPARAM wp, LPARAM lp);
    Layout layout(float width, bool ready) const;
    float origin(const Layout& l, float height) const;
    int hit_test(float x, float y) const;
    void activate(int id);
    void show_more_menu();
    void show_gesture_menu(size_t index);
    int popup(HMENU menu, int anchor);
    void apply_theme();
    void fit_to_screen();
    bool animating() const;
    void set_gauge(Gauge& g, int level, bool charging);
    void set_hover(int id);
    void draw_hero(Painter& p, const Layout& l);
    void draw_gauge(Painter& p, size_t i, const D2D1_RECT_F& box);
    void draw_sound(Painter& p, const Layout& l);
    void draw_touch(Painter& p, const Layout& l);
    void draw_find(Painter& p, const Layout& l);

    Buds& buds_;
    Actions actions_;
    gfx::Theme theme_;
    HWND hwnd_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    float scale_ = 1;  // pixels per DIP
    bool timer_ = false;

    std::array<Gauge, 3> gauges_;
    float switch_from_ = 0, switch_to_ = 0;
    ULONGLONG switch_start_ = 0;
    uint8_t touch_dev_ = model::kLeft;

    std::vector<Box> boxes_;  // clickable areas from the last paint, content coordinates
    float origin_ = 0;        // content y at the window's top edge, from the last paint
    float scroll_ = 0;
    float content_h_ = 0;
    int hover_ = kNone;
    int pressed_ = kNone;
    bool tracking_ = false;
};
