#include "window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "buds.h"
#include "settings.h"
#include "util.h"

using gfx::alpha;
using gfx::rgb;

namespace {

constexpr wchar_t kClass[] = L"NordBudsWindow";
constexpr float kPi = 3.14159265f;
constexpr float kWidth = 440;  // default client width, DIPs
constexpr float kMargin = 18, kTop = 12, kSection = 22, kHeroPad = 20, kRing = 104;
constexpr UINT_PTR kAnimTimer = 1;

D2D1_RECT_F rect(float x, float y, float w, float h) {
    return D2D1::RectF(x, y, x + w, y + h);
}

bool inside(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// c composited over an opaque base colour.
D2D1_COLOR_F over(D2D1_COLOR_F base, D2D1_COLOR_F c) {
    return D2D1::ColorF(base.r + (c.r - base.r) * c.a, base.g + (c.g - base.g) * c.a, base.b + (c.b - base.b) * c.a);
}

float ease_out(float t) {
    return 1 - std::pow(1 - t, 3.0f);
}

// Illustrative response shapes for the EQ tiles (y: 0 bottom .. 1 top, low to high frequencies).
const float* eq_curve(int id) {
    static const float balanced[5] = {0.48f, 0.54f, 0.50f, 0.54f, 0.48f};
    static const float bold[5] = {0.80f, 0.46f, 0.30f, 0.62f, 0.88f};
    static const float bass[5] = {0.95f, 0.80f, 0.42f, 0.30f, 0.30f};
    if (id == 1) return bold;
    if (id == 2) return bass;
    return balanced;
}

const wchar_t* eq_blurb(int id) {
    if (id == 1) return L"Punchy";
    if (id == 2) return L"Deeper lows";
    return L"Natural";
}

std::wstring status_text(const Buds& buds) {
    const auto& st = buds.state();
    if (!st.radio) return L"Bluetooth is off";
    if (st.disconnecting) return L"Disconnecting…";
    switch (st.link) {
    case LinkState::NoDevice: return L"No earbuds paired";
    case LinkState::Disconnected: return L"Not connected";
    case LinkState::Connecting: return st.bt_connected ? L"Connecting…" : L"Not connected";
    case LinkState::Connected: return buds.ready() ? L"Connected" : L"Reading settings…";
    }
    return L"";
}

bool busy(const Buds& buds) {
    const auto& st = buds.state();
    return (st.bt_connected && !buds.ready()) || st.disconnecting;
}

std::pair<std::wstring, std::wstring> empty_text(const Buds& buds) {
    const auto& st = buds.state();
    if (!st.radio) return {L"Bluetooth is off", L"Turn Bluetooth on in\nSettings → Bluetooth & devices."};
    if (st.link == LinkState::NoDevice)
        return {L"No earbuds paired", L"Pair your OnePlus Nord Buds in\nSettings → Bluetooth & devices."};
    if (st.disconnecting) return {L"Disconnecting…", L"Waiting for Windows to let go\nof the earbuds."};
    if (st.bt_connected) return {L"Connecting…", L"Opening the control channel\nto your earbuds."};
    return {L"Not connected", L"Take the earbuds out of the case,\nor press Connect."};
}

bool show_error(const Buds& buds) {
    const auto& st = buds.state();
    return !st.last_error.empty() && !buds.ready() && st.bt_connected;
}

IDWriteTextLayout* make_layout(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight, float width,
                               bool wrap = true) {
    IDWriteTextLayout* layout = nullptr;
    gfx::dwrite()->CreateTextLayout(s.c_str(), UINT32(s.size()), gfx::font(size, weight), std::max(width, 1.0f), 10000,
                                    &layout);
    if (layout && !wrap) layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return layout;
}

D2D1_SIZE_F measure(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight, float width = 10000,
                    bool wrap = true) {
    IDWriteTextLayout* layout = make_layout(s, size, weight, width, wrap);
    if (!layout) return {0, 0};
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    layout->Release();
    return {m.widthIncludingTrailingWhitespace, m.height};
}

ID2D1StrokeStyle* round_caps() {
    static ID2D1StrokeStyle* style = [] {
        ID2D1StrokeStyle* s = nullptr;
        gfx::d2d()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                  D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                      nullptr, 0, &s);
        return s;
    }();
    return style;
}

ID2D1PathGeometry* arc_path(float cx, float cy, float r, float a0, float a1) {
    ID2D1PathGeometry* path = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(gfx::d2d()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) {
        if (path) path->Release();
        return nullptr;
    }
    // A fine polyline rather than AddArc: same look, and Wine's Direct2D (used for screenshots) has no arcs.
    // Figures that are only stroked still begin "filled": Wine skips strokes of hollow figures.
    int n = std::max(2, int(std::abs(a1 - a0) * r / 2));
    sink->BeginFigure(D2D1::Point2F(cx + r * std::cos(a0), cy + r * std::sin(a0)), D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i <= n; i++) {
        float a = a0 + (a1 - a0) * float(i) / float(n);
        sink->AddLine(D2D1::Point2F(cx + r * std::cos(a), cy + r * std::sin(a)));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    return path;
}

}  // namespace

// Drawing helpers bound to one render target, one brush and the theme. fade dims everything
// drawn while it is below 1 (used for disabled controls).
struct MainWindow::Painter {
    ID2D1RenderTarget* rt;
    const gfx::Theme& theme;
    ID2D1SolidColorBrush* brush = nullptr;
    float fade = 1;

    Painter(ID2D1RenderTarget* target, const gfx::Theme& t) : rt(target), theme(t) {
        rt->CreateSolidColorBrush(t.fg, &brush);
    }
    ~Painter() {
        if (brush) brush->Release();
    }
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    ID2D1SolidColorBrush* color(D2D1_COLOR_F c) {
        c.a *= fade;
        brush->SetColor(c);
        return brush;
    }
    D2D1_COLOR_F fg(float a) const { return alpha(theme.fg, a); }

    void fill_round(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), color(c));
    }
    void stroke_round(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width = 1) {
        D2D1_RECT_F in = D2D1::RectF(r.left + width / 2, r.top + width / 2, r.right - width / 2, r.bottom - width / 2);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(in, radius - width / 2, radius - width / 2), color(c), width);
    }
    void circle(float cx, float cy, float r, D2D1_COLOR_F c) {
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), color(c));
    }
    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width) {
        rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), color(c), width, round_caps());
    }
    void arc(float cx, float cy, float r, float a0, float a1, D2D1_COLOR_F c, float width, bool round = true) {
        if (ID2D1PathGeometry* g = arc_path(cx, cy, r, a0, a1)) {
            rt->DrawGeometry(g, color(c), width, round ? round_caps() : nullptr);
            g->Release();
        }
    }
    // Text in r. valign: 0 top, 0.5 centre, 1 bottom.
    void text(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight, const D2D1_RECT_F& r, D2D1_COLOR_F c,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, float valign = 0, bool wrap = true) {
        IDWriteTextLayout* layout = make_layout(s, size, weight, r.right - r.left, wrap);
        if (!layout) return;
        layout->SetTextAlignment(align);
        if (!wrap) {
            DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            IDWriteInlineObject* sign = nullptr;
            gfx::dwrite()->CreateEllipsisTrimmingSign(gfx::font(size, weight), &sign);
            layout->SetTrimming(&trim, sign);
            if (sign) sign->Release();
        }
        DWRITE_TEXT_METRICS m;
        layout->GetMetrics(&m);
        float y = r.top + (r.bottom - r.top - m.height) * valign;
        rt->DrawTextLayout(D2D1::Point2F(r.left, y), layout, color(c));
        layout->Release();
    }
    ID2D1LinearGradientBrush* gradient(D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_COLOR_F c0, D2D1_COLOR_F c1,
                                       float end = 1) {
        c0.a *= fade;
        c1.a *= fade;
        D2D1_GRADIENT_STOP stops[2] = {{0, c0}, {end, c1}};
        ID2D1GradientStopCollection* coll = nullptr;
        ID2D1LinearGradientBrush* g = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, &coll))) {
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(p0, p1), coll, &g);
            coll->Release();
        }
        return g;
    }
};

MainWindow::MainWindow(Buds& buds, Actions actions) : buds_(buds), actions_(std::move(actions)) {
    theme_ = gfx::system_theme();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(0, kClass, L"Nord Buds", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 480, 900,
                            nullptr, nullptr, inst, this);
    apply_theme();
    fit_to_screen();
    refresh();
}

MainWindow::~MainWindow() {
    if (rt_) rt_->Release();
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

void MainWindow::present() {
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    else ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void MainWindow::fit_to_screen() {
    UINT dpi = GetDpiForWindow(hwnd_);
    scale_ = dpi / 96.0f;
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    // Tall enough for everything when connected, but never taller than the screen.
    float want = content_height(kWidth);
    float max_h = (work.bottom - work.top) * 0.92f / scale_;
    RECT r = {0, 0, LONG(kWidth * scale_), LONG(std::min(want, max_h) * scale_)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void MainWindow::apply_theme() {
    BOOL dark = theme_.dark;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    // Windows 11: paint the title bar in the window colour so it reads as one surface. Ignored on Windows 10.
    COLORREF caption = RGB(BYTE(theme_.bg.r * 255), BYTE(theme_.bg.g * 255), BYTE(theme_.bg.b * 255));
    DwmSetWindowAttribute(hwnd_, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    COLORREF text = RGB(BYTE(theme_.fg.r * 255), BYTE(theme_.fg.g * 255), BYTE(theme_.fg.b * 255));
    DwmSetWindowAttribute(hwnd_, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof text);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::set_gauge(Gauge& g, int level, bool charging) {
    if (g.level == level && g.charging == charging) return;
    g.level = level;
    g.charging = charging;
    g.from = g.shown;
    g.start = GetTickCount64();
    g.animating = true;
}

void MainWindow::refresh() {
    const auto& st = buds_.state();
    bool ready = buds_.ready();
    // Battery is only meaningful while the control channel is up; otherwise it reads as unknown.
    const proto::BatteryPart* parts[3] = {&st.battery.left, &st.battery.right, &st.battery.box};
    for (size_t i = 0; i < 3; i++) {
        int level = ready ? parts[i]->level : -1;
        set_gauge(gauges_[i], level, ready && parts[i]->charging && level >= 0);
    }
    float target = st.game_mode == 1 ? 1.0f : 0.0f;
    if (target != switch_to_) {
        ULONGLONG now = GetTickCount64();
        float t = std::min(1.0f, float(now - switch_start_) / 160.0f);
        switch_from_ = switch_from_ + (switch_to_ - switch_from_) * t;
        switch_to_ = target;
        switch_start_ = now;
    }
    if (!ready) scroll_ = 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::animating() const {
    if (!IsWindowVisible(hwnd_)) return false;
    for (const auto& g : gauges_)
        if (g.animating) return true;
    return busy(buds_) || buds_.state().finding || GetTickCount64() - switch_start_ < 200;
}

MainWindow::Layout MainWindow::layout(float w, bool ready) const {
    Layout L;
    L.ready = ready;
    const auto& st = buds_.state();
    const float x0 = kMargin, cw = w - 2 * kMargin;
    float y = kTop;

    // Status card.
    const float ix = x0 + kHeroPad, iw = cw - 2 * kHeroPad, iy = y + kHeroPad;
    L.more = rect(ix + iw - 34, iy + 8, 34, 34);
    bool show_connect = st.link != LinkState::NoDevice;
    L.connect_label = st.bt_connected && !st.disconnecting ? L"Disconnect" : L"Connect";
    if (show_connect) {
        float pill = measure(L.connect_label, 13, DWRITE_FONT_WEIGHT_BOLD).width + 30;
        L.connect = rect(L.more.left - 8 - pill, iy + 8, pill, 34);
    }
    float text_right = (show_connect ? L.connect.left : L.more.left) - 12;
    L.name = D2D1::RectF(ix, iy, text_right, iy + 28);
    L.status = D2D1::RectF(ix + 16, iy + 30, text_right, iy + 50);
    float by = iy + 50 + 16;
    float body_h;
    if (ready) {
        float col = iw / 3;
        for (size_t i = 0; i < 3; i++) L.rings[i] = rect(ix + col * float(i) + (col - kRing) / 2, by, kRing, kRing);
        body_h = kRing + 6 + 18 + 16;
    } else {
        body_h = 8 + 72 + 12 + 24 + 6 + measure(empty_text(buds_).second, 13, DWRITE_FONT_WEIGHT_NORMAL, iw).height + 8;
    }
    L.body = rect(ix, by, iw, body_h);
    float hy = by + body_h;
    if (show_error(buds_)) {
        float th = measure(util::wide(st.last_error), 12, DWRITE_FONT_WEIGHT_NORMAL, iw - 24).height;
        L.error = rect(ix, hy + 14, iw, th + 16);
        hy = L.error.bottom;
    }
    L.hero = D2D1::RectF(x0, y, x0 + cw, hy + 18);
    y = L.hero.bottom + kSection;
    if (!ready) {
        L.height = y + 2;
        return L;
    }

    // Sound: three preset tiles and the game mode row.
    L.sound_head = rect(x0 + 8, y, cw - 16, 20);
    y += 28;
    float card_top = y;
    float tw = (cw - 28 - 20) / 3;
    for (size_t i = 0; i < 3; i++) L.tiles[i] = rect(x0 + 14 + float(i) * (tw + 10), y + 14, tw, 104);
    L.game_row = rect(x0, y + 14 + 104 + 14, cw, 62);
    L.game_switch = rect(x0 + cw - 16 - 40, L.game_row.top + 21, 40, 20);
    L.sound_card = D2D1::RectF(x0, card_top, x0 + cw, L.game_row.bottom);
    y = L.sound_card.bottom + kSection;

    // Touch controls: side switch and one row per gesture.
    L.touch_head = rect(x0 + 8, y, cw - 16, 20);
    y += 28;
    card_top = y;
    L.seg = rect(x0 + 14, y + 14, cw - 28, 38);
    float half = (L.seg.right - L.seg.left - 6) / 2;
    L.sides[0] = rect(L.seg.left + 3, L.seg.top + 3, half, 32);
    L.sides[1] = rect(L.seg.left + 3 + half, L.seg.top + 3, half, 32);
    float ry = L.seg.bottom + 6;
    for (size_t i = 0; i < model::gesture_slots().size(); i++) {
        L.gesture_rows.push_back(rect(x0, ry, cw, 58));
        L.gesture_controls.push_back(rect(x0 + cw - 16 - 168, ry + 12, 168, 34));
        ry += 58;
    }
    L.touch_card = D2D1::RectF(x0, card_top, x0 + cw, ry + 6);
    y = L.touch_card.bottom + kSection;

    // Find: ring button beside a title and a note. Sized for the longer of its two notes.
    L.find_head = rect(x0 + 8, y, cw - 16, 20);
    y += 28;
    float text_x = x0 + 16 + 52 + 16, text_w = x0 + cw - 16 - text_x;
    float note_h = 0;
    for (const wchar_t* note : {L"Plays a loud tone on both earbuds. Take them out of your ears first.",
                                L"Press again to stop. Stops by itself after a minute."})
        note_h = std::max(note_h, measure(note, 12, DWRITE_FONT_WEIGHT_NORMAL, text_w).height);
    float th = 20 + 2 + note_h;
    float row_h = std::max(52.0f, th) + 36;
    L.find_card = rect(x0, y, cw, row_h);
    L.find_button = rect(x0 + 16, y + (row_h - 52) / 2, 52, 52);
    L.find_title = rect(text_x, y + (row_h - th) / 2, text_w, 20);
    L.find_body = rect(text_x, L.find_title.bottom + 2, text_w, note_h);
    L.height = L.find_card.bottom + 24;
    return L;
}

float MainWindow::content_height(float width) const {
    return layout(width, true).height;
}

float MainWindow::origin(const Layout& l, float height) const {
    // With only the status card to show, keep it centred instead of stranded at the top.
    if (!l.ready) return std::max(0.0f, (height - l.height) / 2);
    return -scroll_;
}

void MainWindow::paint(ID2D1RenderTarget* rt, float w, float h) {
    Painter p(rt, theme_);
    rt->Clear(theme_.bg);
    if (!p.brush) return;
    const Layout L = layout(w, buds_.ready());
    content_h_ = L.height;
    float max_scroll = std::max(0.0f, L.height - h);
    scroll_ = std::clamp(scroll_, 0.0f, max_scroll);
    origin_ = origin(L, h);

    const auto& st = buds_.state();
    boxes_.clear();
    if (st.link != LinkState::NoDevice) boxes_.push_back({L.connect, kConnect});
    boxes_.push_back({L.more, kMore});
    if (L.ready) {
        if (buds_.supports(proto::kEqSet))
            for (size_t i = 0; i < 3; i++) boxes_.push_back({L.tiles[i], kEq + model::eq_presets()[i].id});
        if (buds_.supports(proto::kFeatureSet) && st.game_mode >= 0) boxes_.push_back({L.game_row, kGame});
        boxes_.push_back({L.sides[0], kSideLeft});
        boxes_.push_back({L.sides[1], kSideRight});
        bool can_touch = !st.gestures.empty() && buds_.supports(proto::kGesturesSet);
        for (size_t i = 0; i < L.gesture_controls.size(); i++)
            if (can_touch && model::gesture_slots()[i].options.size() > 1)
                boxes_.push_back({L.gesture_controls[i], kGesture + int(i)});
        if (buds_.supports(proto::kFind)) boxes_.push_back({L.find_button, kFind});
    }

    D2D1::Matrix3x2F base;
    rt->GetTransform(&base);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, origin_) * base);
    draw_hero(p, L);
    if (L.ready) {
        draw_sound(p, L);
        draw_touch(p, L);
        draw_find(p, L);
    }
    rt->SetTransform(base);

    if (max_scroll > 0) {
        float thumb = std::max(40.0f, h * h / L.height);
        float ty = (h - thumb) * scroll_ / max_scroll;
        p.fill_round(rect(w - 6, ty + 2, 3, thumb - 4), 1.5f, p.fg(0.22f));
    }
}

void MainWindow::draw_hero(Painter& p, const Layout& L) {
    const auto& st = buds_.state();
    const auto& t = theme_;
    ULONGLONG now = GetTickCount64();

    // Card: accent wash at the top left fading out, thin outline.
    float cw = L.hero.right - L.hero.left, ch = L.hero.bottom - L.hero.top;
    float dx = std::sin(155 * kPi / 180), dy = -std::cos(155 * kPi / 180);
    float len = std::abs(cw * dx) + std::abs(ch * dy);
    D2D1_POINT_2F c = D2D1::Point2F(L.hero.left + cw / 2, L.hero.top + ch / 2);
    if (ID2D1LinearGradientBrush* g =
            p.gradient(D2D1::Point2F(c.x - dx * len / 2, c.y - dy * len / 2),
                       D2D1::Point2F(c.x + dx * len / 2, c.y + dy * len / 2), over(t.bg, alpha(t.accent, t.dark ? 0.20f : 0.16f)),
                       over(t.bg, alpha(t.accent, 0.03f)), 0.7f)) {
        p.rt->FillRoundedRectangle(D2D1::RoundedRect(L.hero, 24, 24), g);
        g->Release();
    }
    p.stroke_round(L.hero, 24, p.fg(0.07f));

    std::wstring name = st.name.empty() ? L"OnePlus Nord Buds 2r" : util::wide(st.name);
    p.text(name, 18, DWRITE_FONT_WEIGHT_BOLD, L.name, t.fg, DWRITE_TEXT_ALIGNMENT_LEADING, 0.5f, false);

    // Status dot: green when ready, blinking amber while busy.
    float dot_y = (L.status.top + L.status.bottom) / 2;
    float dot_x = L.status.left - 12;
    if (buds_.ready()) {
        p.circle(dot_x, dot_y, 7, alpha(rgb(0x2ec27e), 0.22f));
        p.circle(dot_x, dot_y, 4, rgb(0x2ec27e));
    } else if (busy(buds_)) {
        float k = 0.25f + 0.75f * (0.5f + 0.5f * std::cos(2 * kPi * float(now % 1100) / 1100.0f));
        p.circle(dot_x, dot_y, 4, alpha(rgb(0xe5a50a), k));
    } else {
        p.circle(dot_x, dot_y, 4, p.fg(0.35f));
    }
    p.text(status_text(buds_), 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L.status, p.fg(0.75f),
           DWRITE_TEXT_ALIGNMENT_LEADING, 0.5f, false);

    if (st.link != LinkState::NoDevice) {
        bool suggested = !(st.bt_connected && !st.disconnecting);
        bool hot = hover_ == kConnect;
        if (suggested) {
            p.fill_round(rect(L.connect.left, L.connect.top + 2, L.connect.right - L.connect.left, 34), 17,
                         alpha(t.accent, 0.25f));
            p.fill_round(L.connect, 17, hot ? alpha(t.accent, 0.88f) : t.accent);
        } else {
            p.fill_round(L.connect, 17, p.fg(hot ? 0.14f : 0.08f));
        }
        p.text(L.connect_label, 13, DWRITE_FONT_WEIGHT_BOLD, L.connect, suggested ? t.on_accent : t.fg,
               DWRITE_TEXT_ALIGNMENT_CENTER, 0.5f, false);
    }

    p.circle((L.more.left + L.more.right) / 2, (L.more.top + L.more.bottom) / 2, 17, p.fg(hover_ == kMore ? 0.13f : 0.06f));
    for (int i = -1; i <= 1; i++)
        p.circle((L.more.left + L.more.right) / 2 + float(i) * 6, (L.more.top + L.more.bottom) / 2, 1.8f, p.fg(0.85f));

    if (L.ready) {
        const wchar_t* names[3] = {L"Left", L"Right", L"Case"};
        const proto::BatteryPart* parts[3] = {&st.battery.left, &st.battery.right, &st.battery.box};
        for (size_t i = 0; i < 3; i++) {
            const D2D1_RECT_F& ring = L.rings[i];
            draw_gauge(p, i, ring);
            float col_l = ring.left - 30, col_r = ring.right + 30;
            p.text(names[i], 13, DWRITE_FONT_WEIGHT_BOLD, D2D1::RectF(col_l, ring.bottom + 6, col_r, ring.bottom + 24),
                   t.fg, DWRITE_TEXT_ALIGNMENT_CENTER);
            const Gauge& g = gauges_[i];
            bool low = !g.charging && g.level >= 0 && g.level < 20;
            std::wstring note;
            D2D1_COLOR_F color = p.fg(0.6f);
            auto weight = DWRITE_FONT_WEIGHT_NORMAL;
            if (g.charging) {
                note = L"Charging";
                color = t.blue;
                weight = DWRITE_FONT_WEIGHT_BOLD;
            } else if (low) {
                note = L"Low";
                color = rgb(t.dark ? 0xf66151 : 0xc01c28);
                weight = DWRITE_FONT_WEIGHT_BOLD;
            } else if (parts[i]->level < 0) {
                note = i == 2 ? L"Open the lid" : L"Not reported";
            }
            p.text(note, 11, weight, D2D1::RectF(col_l, ring.bottom + 24, col_r, ring.bottom + 40), color,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    } else {
        auto [title, body] = empty_text(buds_);
        float cx = (L.body.left + L.body.right) / 2;
        float y = L.body.top + 8;
        p.brush->SetColor(p.fg(0.4f));
        gfx::draw_buds(p.rt, cx - 36, y, 72, p.brush);
        y += 72 + 12;
        bool spin = busy(buds_);
        float tw = measure(title, 17, DWRITE_FONT_WEIGHT_BOLD, 10000, false).width;
        float total = tw + (spin ? 24 : 0);
        float x = cx - total / 2;
        if (spin) {
            float a0 = 2 * kPi * float(now % 1000) / 1000.0f;
            p.arc(x + 8, y + 12, 7, a0, a0 + 1.5f * kPi, p.fg(0.8f), 2);
            x += 24;
        }
        p.text(title, 17, DWRITE_FONT_WEIGHT_BOLD, rect(x, y, tw + 2, 24), t.fg, DWRITE_TEXT_ALIGNMENT_LEADING, 0.5f,
               false);
        y += 24 + 6;
        p.text(body, 13, DWRITE_FONT_WEIGHT_NORMAL, D2D1::RectF(L.body.left, y, L.body.right, L.body.bottom), p.fg(0.65f),
               DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    if (show_error(buds_)) {
        D2D1_COLOR_F red = rgb(t.dark ? 0xff7b72 : 0xc01c28);
        p.fill_round(L.error, 12, alpha(t.red, t.dark ? 0.16f : 0.08f));
        p.text(util::wide(st.last_error), 12, DWRITE_FONT_WEIGHT_NORMAL,
               D2D1::RectF(L.error.left + 12, L.error.top + 8, L.error.right - 12, L.error.bottom - 8), red);
    }
}

void MainWindow::draw_gauge(Painter& p, size_t i, const D2D1_RECT_F& box) {
    Gauge& g = gauges_[i];
    if (g.animating) {
        float t = std::min(1.0f, float(GetTickCount64() - g.start) / 700.0f);
        g.shown = g.from + (float(std::max(g.level, 0)) - g.from) * ease_out(t);
        if (t >= 1) g.animating = false;
    }
    const float line = 9;
    const float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    const float radius = (box.right - box.left) / 2 - line;
    const float start = -kPi / 2;

    p.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), p.color(p.fg(g.level < 0 ? 0.08f : 0.11f)),
                      line);
    if (g.level >= 0 && g.shown > 0.2f) {
        auto [c1, c2] = gfx::level_colors(g.level, g.charging);
        float frac = std::min(g.shown, 100.0f) / 100.0f;
        float end = start + frac * 2 * kPi;
        if (frac >= 0.999f) end = start + 2 * kPi - 0.001f;
        // Soft glow under the arc, then the arc itself shaded from c1 to c2 along its length.
        p.arc(cx, cy, radius, start, end, alpha(c2, 0.16f), line + 7);
        // A solid arc underneath hides the seams between the shaded pieces.
        D2D1_COLOR_F mid = D2D1::ColorF((c1.r + c2.r) / 2, (c1.g + c2.g) / 2, (c1.b + c2.b) / 2);
        p.arc(cx, cy, radius, start, end, mid, line, false);
        int steps = std::max(8, int(frac * 120));
        for (int s = 0; s < steps; s++) {
            float a0 = start + (end - start) * float(s) / float(steps);
            float a1 = std::min(end, start + (end - start) * float(s + 1) / float(steps) + 0.02f);
            float k = float(s) / float(steps);
            D2D1_COLOR_F c = D2D1::ColorF(c1.r + (c2.r - c1.r) * k, c1.g + (c2.g - c1.g) * k, c1.b + (c2.b - c1.b) * k);
            p.arc(cx, cy, radius, a0, a1, c, line, false);
        }
        p.circle(cx + radius * std::cos(start), cy + radius * std::sin(start), line / 2, c1);
        p.circle(cx + radius * std::cos(end), cy + radius * std::sin(end), line / 2, c2);
    }

    // Number with a smaller "%" on the same baseline.
    std::wstring number = g.level < 0 ? L"–" : std::to_wstring(int(std::lround(g.shown)));
    IDWriteTextLayout* big = make_layout(number, 25, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 200, false);
    IDWriteTextLayout* unit = g.level < 0 ? nullptr : make_layout(L"%", 13, DWRITE_FONT_WEIGHT_BOLD, 50, false);
    if (!big) return;
    DWRITE_TEXT_METRICS bm, um = {};
    big->GetMetrics(&bm);
    if (unit) unit->GetMetrics(&um);
    float x = cx - (bm.widthIncludingTrailingWhitespace + um.widthIncludingTrailingWhitespace) / 2;
    float y = cy - bm.height / 2;
    p.rt->DrawTextLayout(D2D1::Point2F(x, y), big, p.color(p.fg(g.level < 0 ? 0.45f : 1.0f)));
    if (unit) {
        DWRITE_LINE_METRICS bl, ul;
        UINT32 n = 0;
        big->GetLineMetrics(&bl, 1, &n);
        unit->GetLineMetrics(&ul, 1, &n);
        p.rt->DrawTextLayout(D2D1::Point2F(x + bm.widthIncludingTrailingWhitespace, y + bl.baseline - ul.baseline), unit,
                             p.color(p.fg(0.6f)));
        unit->Release();
    }
    big->Release();
}

void MainWindow::draw_sound(Painter& p, const Layout& L) {
    const auto& st = buds_.state();
    const auto& t = theme_;
    p.text(L"Sound", 14, DWRITE_FONT_WEIGHT_EXTRA_BOLD, L.sound_head, p.fg(0.8f));
    p.fill_round(L.sound_card, 18, p.fg(0.045f));
    p.stroke_round(L.sound_card, 18, p.fg(0.05f));

    bool can_eq = buds_.supports(proto::kEqSet);
    for (size_t i = 0; i < 3; i++) {
        const auto& preset = model::eq_presets()[i];
        const D2D1_RECT_F& r = L.tiles[i];
        bool checked = st.eq == preset.id;
        bool hot = hover_ == kEq + preset.id;
        p.fade = can_eq ? 1.0f : 0.5f;
        if (checked) {
            p.fill_round(r, 16, alpha(t.accent, 0.15f));
            p.stroke_round(r, 16, t.accent, 2);
        } else {
            p.fill_round(r, 16, p.fg(hot ? 0.09f : 0.05f));
        }

        // Curve through five points (Catmull-Rom as cubic Béziers), filled with a fade underneath.
        const float* yv = eq_curve(preset.id);
        D2D1_RECT_F area = D2D1::RectF(r.left + 8, r.top + 12, r.right - 8, r.top + 52);
        auto px = [&](int k) { return area.left + 4 + (area.right - area.left - 8) * float(k) / 4; };
        auto py = [&](int k) { return area.top + 4 + (area.bottom - area.top - 8) * (1 - yv[k]); };
        D2D1_COLOR_F ink = checked ? t.accent : p.fg(0.45f);
        // Gradients use solid colours mixed by hand: the tile colour under the curve, and the ink over it.
        D2D1_COLOR_F under = over(over(t.bg, p.fg(0.045f)), checked ? alpha(t.accent, 0.15f) : p.fg(hot ? 0.09f : 0.05f));
        for (int pass = 0; pass < 2; pass++) {
            ID2D1PathGeometry* path = nullptr;
            ID2D1GeometrySink* sink = nullptr;
            if (FAILED(gfx::d2d()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) {
                if (path) path->Release();
                continue;
            }
            sink->BeginFigure(D2D1::Point2F(px(0), py(0)), D2D1_FIGURE_BEGIN_FILLED);
            for (int k = 0; k < 4; k++) {
                int k0 = std::max(k - 1, 0), k3 = std::min(k + 2, 4);
                sink->AddBezier(D2D1::BezierSegment(
                    D2D1::Point2F(px(k) + (px(k + 1) - px(k0)) / 6, py(k) + (py(k + 1) - py(k0)) / 6),
                    D2D1::Point2F(px(k + 1) - (px(k3) - px(k)) / 6, py(k + 1) - (py(k3) - py(k)) / 6),
                    D2D1::Point2F(px(k + 1), py(k + 1))));
            }
            if (pass == 0) {
                sink->AddLine(D2D1::Point2F(px(4), area.bottom));
                sink->AddLine(D2D1::Point2F(px(0), area.bottom));
            }
            sink->EndFigure(pass == 0 ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            if (pass == 0) {
                if (ID2D1LinearGradientBrush* g = p.gradient(D2D1::Point2F(0, area.top), D2D1::Point2F(0, area.bottom),
                                                             over(under, alpha(ink, 0.30f)), under)) {
                    p.rt->FillGeometry(path, g);
                    g->Release();
                }
            } else {
                p.rt->DrawGeometry(path, p.color(ink), 2.5f, round_caps());
            }
            path->Release();
        }
        p.text(util::wide(preset.name), 13, DWRITE_FONT_WEIGHT_BOLD, D2D1::RectF(r.left, r.top + 60, r.right, r.top + 78),
               t.fg, DWRITE_TEXT_ALIGNMENT_CENTER);
        p.text(eq_blurb(preset.id), 11, DWRITE_FONT_WEIGHT_NORMAL, D2D1::RectF(r.left, r.top + 78, r.right, r.top + 94),
               p.fg(0.6f), DWRITE_TEXT_ALIGNMENT_CENTER);
        p.fade = 1;
    }

    // Game mode row.
    const D2D1_RECT_F& row = L.game_row;
    bool can_game = buds_.supports(proto::kFeatureSet) && st.game_mode >= 0;
    p.line(row.left, row.top + 0.5f, row.right, row.top + 0.5f, p.fg(0.07f), 1);
    D2D1_RECT_F icon = rect(row.left + 16, row.top + 13, 36, 36);
    p.fill_round(icon, 11, alpha(t.accent, 0.14f));
    {
        // Game controller outline with a d-pad and two buttons.
        float ox = icon.left + 8, oy = icon.top + 8;
        p.stroke_round(D2D1::RectF(ox + 1, oy + 5, ox + 19, oy + 16), 5.5f, t.accent, 1.6f);
        p.line(ox + 4.6f, oy + 10.5f, ox + 8.4f, oy + 10.5f, t.accent, 1.5f);
        p.line(ox + 6.5f, oy + 8.6f, ox + 6.5f, oy + 12.4f, t.accent, 1.5f);
        p.circle(ox + 13.2f, oy + 9.6f, 1.1f, t.accent);
        p.circle(ox + 15.4f, oy + 11.8f, 1.1f, t.accent);
    }
    float tx = icon.right + 12;
    p.text(L"Game mode", 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(tx, row.top + 12, L.game_switch.left - 12, row.top + 32),
           t.fg);
    p.text(L"Lower latency for games and video", 12, DWRITE_FONT_WEIGHT_NORMAL,
           D2D1::RectF(tx, row.top + 32, L.game_switch.left - 12, row.top + 50), p.fg(0.6f));

    // Switch, animated between off (outline) and on (accent fill).
    float k = ease_out(std::min(1.0f, float(GetTickCount64() - switch_start_) / 160.0f));
    float pos = switch_from_ + (switch_to_ - switch_from_) * k;
    const D2D1_RECT_F& sw = L.game_switch;
    p.fade = can_game ? 1.0f : 0.4f;
    if (hover_ == kGame && can_game) p.fill_round(sw, 10, p.fg(0.06f));
    p.fill_round(sw, 10, alpha(t.accent, pos));
    p.stroke_round(sw, 10, p.fg(0.55f * (1 - pos)));
    D2D1_COLOR_F off = p.fg(0.8f), on = t.on_accent;
    D2D1_COLOR_F knob = D2D1::ColorF(off.r + (on.r - off.r) * pos, off.g + (on.g - off.g) * pos,
                                     off.b + (on.b - off.b) * pos, off.a + (on.a - off.a) * pos);
    p.circle(sw.left + 10 + 20 * pos, (sw.top + sw.bottom) / 2, 6 + pos, knob);
    p.fade = 1;
}

void MainWindow::draw_touch(Painter& p, const Layout& L) {
    const auto& st = buds_.state();
    const auto& t = theme_;
    p.text(L"Touch controls", 14, DWRITE_FONT_WEIGHT_EXTRA_BOLD, L.touch_head, p.fg(0.8f));
    p.fill_round(L.touch_card, 18, p.fg(0.045f));
    p.stroke_round(L.touch_card, 18, p.fg(0.05f));

    // Left / right segmented switch.
    p.fill_round(L.seg, 12, p.fg(0.06f));
    const wchar_t* sides[2] = {L"Left earbud", L"Right earbud"};
    for (size_t i = 0; i < 2; i++) {
        bool on = (i == 0) == (touch_dev_ == model::kLeft);
        const D2D1_RECT_F& r = L.sides[i];
        if (on) {
            p.fill_round(rect(r.left, r.top + 1, r.right - r.left, r.bottom - r.top), 9, rgb(0, t.dark ? 0.35f : 0.12f));
            p.fill_round(r, 9, t.raised);
        } else if (hover_ == (i == 0 ? kSideLeft : kSideRight)) {
            p.fill_round(r, 9, p.fg(0.05f));
        }
        p.text(sides[i], 13, DWRITE_FONT_WEIGHT_BOLD, r, on ? t.fg : p.fg(0.7f), DWRITE_TEXT_ALIGNMENT_CENTER, 0.5f, false);
    }

    bool can_touch = !st.gestures.empty() && buds_.supports(proto::kGesturesSet);
    const auto& slots = model::gesture_slots();
    for (size_t i = 0; i < slots.size(); i++) {
        const auto& slot = slots[i];
        const D2D1_RECT_F& row = L.gesture_rows[i];
        const D2D1_RECT_F& ctl = L.gesture_controls[i];
        float cy = (row.top + row.bottom) / 2;

        // Chip: one dot per tap, or HOLD.
        D2D1_RECT_F chip = rect(row.left + 16, cy - 15, 50, 30);
        p.fill_round(chip, 10, alpha(t.accent, t.dark ? 0.16f : 0.12f));
        if (slot.act >= 1 && slot.act <= 3) {
            int n = slot.act;
            float x = (chip.left + chip.right) / 2 - float(n - 1) * 5;
            for (int d = 0; d < n; d++) p.circle(x + float(d) * 10, cy, 3.2f, t.accent);
        } else {
            p.text(L"HOLD", 9.5f, DWRITE_FONT_WEIGHT_EXTRA_BOLD, chip, t.accent, DWRITE_TEXT_ALIGNMENT_CENTER, 0.5f, false);
        }
        p.text(util::wide(slot.name), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(chip.right + 12, row.top, ctl.left - 8, row.bottom),
               t.fg, DWRITE_TEXT_ALIGNMENT_LEADING, 0.5f, false);

        if (slot.options.size() <= 1) {
            p.text(util::wide(model::function_name(slot.options.front())), 13, DWRITE_FONT_WEIGHT_NORMAL, ctl, p.fg(0.6f),
                   DWRITE_TEXT_ALIGNMENT_TRAILING, 0.5f, false);
            continue;
        }
        std::wstring current = L"–";
        for (const auto& e : st.gestures)
            if (e.dev == touch_dev_ && e.act == slot.act) current = util::wide(model::function_name(e.fn));
        p.fade = can_touch ? 1.0f : 0.45f;
        bool hot = can_touch && hover_ == kGesture + int(i);
        p.fill_round(ctl, 10, p.fg(hot ? 0.10f : 0.06f));
        p.stroke_round(ctl, 10, p.fg(0.08f));
        p.text(current, 13, DWRITE_FONT_WEIGHT_NORMAL, D2D1::RectF(ctl.left + 12, ctl.top, ctl.right - 30, ctl.bottom), t.fg,
               DWRITE_TEXT_ALIGNMENT_LEADING, 0.5f, false);
        float chx = ctl.right - 16, chy = (ctl.top + ctl.bottom) / 2;
        p.line(chx - 4, chy - 2, chx, chy + 2, p.fg(0.7f), 1.6f);
        p.line(chx, chy + 2, chx + 4, chy - 2, p.fg(0.7f), 1.6f);
        p.fade = 1;
    }
}

void MainWindow::draw_find(Painter& p, const Layout& L) {
    const auto& st = buds_.state();
    const auto& t = theme_;
    p.text(L"Find my earbuds", 14, DWRITE_FONT_WEIGHT_EXTRA_BOLD, L.find_head, p.fg(0.8f));
    p.fill_round(L.find_card, 18, p.fg(0.045f));
    p.stroke_round(L.find_card, 18, p.fg(0.05f));

    const D2D1_RECT_F& b = L.find_button;
    float cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
    p.fade = buds_.supports(proto::kFind) ? 1.0f : 0.45f;
    D2D1_COLOR_F fill = st.finding ? t.red : t.accent;
    D2D1_COLOR_F ink = st.finding ? rgb(0xffffff) : t.on_accent;
    if (st.finding) {
        float k = float(GetTickCount64() % 1300) / 1300.0f;
        p.circle(cx, cy, 26 + 16 * k, alpha(t.red, 0.55f * (1 - k)));
    }
    p.circle(cx, cy, 26, hover_ == kFind ? alpha(fill, 0.88f) : fill);
    if (st.finding) {
        p.fill_round(rect(cx - 7, cy - 7, 14, 14), 3, ink);
    } else {
        // Speaker with two sound waves.
        ID2D1PathGeometry* path = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(gfx::d2d()->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
            float x = cx - 9, y = cy - 10;
            sink->BeginFigure(D2D1::Point2F(x + 1, y + 7.5f), D2D1_FIGURE_BEGIN_FILLED);
            D2D1_POINT_2F pts[] = {{x + 4.5f, y + 7.5f}, {x + 9, y + 3.5f}, {x + 9, y + 16.5f}, {x + 4.5f, y + 12.5f},
                                   {x + 1, y + 12.5f}};
            sink->AddLines(pts, 5);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            p.rt->FillGeometry(path, p.color(ink));
            path->Release();
            p.arc(x + 9, y + 10, 4, -kPi / 4, kPi / 4, ink, 1.8f);
            p.arc(x + 9, y + 10, 7.5f, -kPi / 4, kPi / 4, ink, 1.8f);
        } else if (path) {
            path->Release();
        }
    }
    p.fade = 1;
    p.text(st.finding ? L"Ringing…" : L"Ring earbuds", 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, L.find_title, t.fg);
    p.text(st.finding ? L"Press again to stop. Stops by itself after a minute."
                      : L"Plays a loud tone on both earbuds. Take them out of your ears first.",
           12, DWRITE_FONT_WEIGHT_NORMAL, L.find_body, p.fg(0.6f));
}

int MainWindow::hit_test(float x, float y) const {
    float cy = y - origin_;
    for (const auto& b : boxes_)
        if (inside(b.rect, x, cy)) return b.id;
    return kNone;
}

void MainWindow::set_hover(int id) {
    if (id == hover_) return;
    hover_ = id;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::activate(int id) {
    const auto& st = buds_.state();
    if (id == kConnect) {
        if (st.bt_connected && !st.disconnecting) buds_.bluetooth_disconnect();
        else buds_.bluetooth_connect();
    } else if (id == kMore) {
        show_more_menu();
    } else if (id == kGame) {
        buds_.set_game_mode(st.game_mode != 1);
    } else if (id == kSideLeft || id == kSideRight) {
        touch_dev_ = id == kSideLeft ? model::kLeft : model::kRight;
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (id == kFind) {
        buds_.set_find(!st.finding);
    } else if (id >= kGesture) {
        show_gesture_menu(size_t(id - kGesture));
    } else if (id >= kEq) {
        if (id - kEq != st.eq) buds_.set_eq(id - kEq);
    }
}

int MainWindow::popup(HMENU menu, int anchor) {
    D2D1_RECT_F r = {};
    for (const auto& b : boxes_)
        if (b.id == anchor) r = b.rect;
    RECT ex = {LONG(r.left * scale_), LONG((r.top + origin_) * scale_), LONG(r.right * scale_),
               LONG((r.bottom + origin_) * scale_)};
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&ex), 2);
    TPMPARAMS tpm = {sizeof tpm, ex};
    // Returns the chosen id; 0 when dismissed.
    return TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_VERTICAL |
                                      (anchor == kMore ? TPM_RIGHTALIGN : TPM_LEFTALIGN),
                            anchor == kMore ? ex.right : ex.left, ex.bottom, hwnd_, &tpm);
}

void MainWindow::show_more_menu() {
    const auto& st = buds_.state();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (Settings::get().autostart() ? MF_CHECKED : 0), 1, L"Start with Windows");
    AppendMenuW(m, MF_STRING | (Settings::get().tray_battery() ? MF_CHECKED : 0), 2, L"Battery level on tray icon");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::wstring fw = L"Firmware " + (st.firmware.summary.empty() ? std::wstring(L"–") : util::wide(st.firmware.summary));
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, fw.c_str());
    if (!st.firmware.left.empty()) {
        std::wstring parts = L"Left " + util::wide(st.firmware.left) + L" · Right " + util::wide(st.firmware.right) +
                             L" · Case " + util::wide(st.firmware.box);
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, parts.c_str());
    }
    std::wstring addr = L"Bluetooth " + (st.address.empty() ? std::wstring(L"–") : util::wide(st.address));
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, addr.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"Bluetooth settings…");
    AppendMenuW(m, MF_STRING, 4, L"Quit Nord Buds");
    int choice = popup(m, kMore);
    DestroyMenu(m);
    if (choice == 1) Settings::get().set_autostart(!Settings::get().autostart());
    else if (choice == 2) Settings::get().set_tray_battery(!Settings::get().tray_battery());
    else if (choice == 3 && actions_.bluetooth_settings) actions_.bluetooth_settings();
    else if (choice == 4 && actions_.quit) actions_.quit();
}

void MainWindow::show_gesture_menu(size_t index) {
    const auto& slot = model::gesture_slots()[index];
    HMENU m = CreatePopupMenu();
    UINT checked = 0;
    for (size_t i = 0; i < slot.options.size(); i++) {
        AppendMenuW(m, MF_STRING, UINT(i + 1), util::wide(model::function_name(slot.options[i])).c_str());
        for (const auto& e : buds_.state().gestures)
            if (e.dev == touch_dev_ && e.act == slot.act && e.fn == slot.options[i]) checked = UINT(i + 1);
    }
    if (checked) CheckMenuRadioItem(m, 1, UINT(slot.options.size()), checked, MF_BYCOMMAND);
    int choice = popup(m, kGesture + int(index));
    DestroyMenu(m);
    if (choice > 0 && UINT(choice) != checked) buds_.set_gesture({touch_dev_}, slot.act, slot.options[size_t(choice - 1)]);
}

LRESULT CALLBACK MainWindow::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<MainWindow*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->on_message(msg, wp, lp);
}

LRESULT MainWindow::on_message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        UINT dpi = GetDpiForWindow(hwnd_);
        scale_ = dpi / 96.0f;
        if (!rt_) {
            auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), float(dpi),
                                                      float(dpi));
            gfx::d2d()->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(UINT(rc.right), UINT(rc.bottom))), &rt_);
        }
        if (rt_) {
            rt_->SetDpi(float(dpi), float(dpi));
            rt_->BeginDraw();
            paint(rt_, float(rc.right) / scale_, float(rc.bottom) / scale_);
            if (rt_->EndDraw() == HRESULT(D2DERR_RECREATE_TARGET)) {
                rt_->Release();
                rt_ = nullptr;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        EndPaint(hwnd_, &ps);
        bool want = animating();
        if (want && !timer_) SetTimer(hwnd_, kAnimTimer, 16, nullptr);
        if (!want && timer_) KillTimer(hwnd_, kAnimTimer);
        timer_ = want;
        return 0;
    }
    case WM_TIMER:
        if (wp == kAnimTimer) InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        UINT dpi = GetDpiForWindow(hwnd_);
        mm->ptMinTrackSize = {LONG(380 * dpi / 96), LONG(420 * dpi / 96)};
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            theme_ = gfx::system_theme();
            apply_theme();
        }
        return 0;
    case WM_SHOWWINDOW:
        if (wp) InvalidateRect(hwnd_, nullptr, FALSE);
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_MOUSEWHEEL: {
        float max_scroll = 0;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        if (buds_.ready()) max_scroll = std::max(0.0f, content_h_ - float(rc.bottom) / scale_);
        scroll_ = std::clamp(scroll_ - float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 60, 0.0f, max_scroll);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd_, 0};
            tracking_ = TrackMouseEvent(&tme);
        }
        set_hover(hit_test(float(GET_X_LPARAM(lp)) / scale_, float(GET_Y_LPARAM(lp)) / scale_));
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        set_hover(kNone);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && hover_ != kNone) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_LBUTTONDOWN:
        pressed_ = hit_test(float(GET_X_LPARAM(lp)) / scale_, float(GET_Y_LPARAM(lp)) / scale_);
        if (pressed_ != kNone) SetCapture(hwnd_);
        return 0;
    case WM_LBUTTONUP: {
        int id = hit_test(float(GET_X_LPARAM(lp)) / scale_, float(GET_Y_LPARAM(lp)) / scale_);
        int was = pressed_;
        pressed_ = kNone;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (id != kNone && id == was) activate(id);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow(hwnd_, SW_HIDE);
        return 0;
    case WM_CLOSE:
        // Keep running in the tray; Quit is in the menus.
        ShowWindow(hwnd_, SW_HIDE);
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}
