#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <string>
#include <utility>

// Direct2D / DirectWrite helpers shared by the window and the tray icon.
namespace gfx {

ID2D1Factory* d2d();
IDWriteFactory* dwrite();
IWICImagingFactory* wic();

struct Theme {
    bool dark = true;
    D2D1_COLOR_F bg, fg, accent, on_accent, raised, red, blue;
};
// Light or dark and the accent colour, as set in Settings → Personalization.
Theme system_theme();
Theme make_theme(bool dark, unsigned accent_rgb);
bool taskbar_is_light();

D2D1_COLOR_F rgb(unsigned hex, float alpha = 1.0f);
D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a);

// Text format for the UI font at a size in DIPs. Cached; do not release.
IDWriteTextFormat* font(float size, DWRITE_FONT_WEIGHT weight);

// Battery arc gradient: blue while charging, then green, amber, red by level.
std::pair<D2D1_COLOR_F, D2D1_COLOR_F> level_colors(int level, bool charging);

// The earbuds glyph (res/nordbuds-symbolic.svg) in a size x size box.
void draw_buds(ID2D1RenderTarget* rt, float x, float y, float size, ID2D1Brush* brush);

struct TrayBadge {
    bool connected = false;
    bool show_level = false;
    int level = -1;  // lowest earbud, -1 = unknown
    bool charging = false;
    bool light_taskbar = false;
};
HICON tray_icon(int size, const TrayBadge& badge);

}  // namespace gfx
