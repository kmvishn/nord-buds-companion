#include "gfx.h"

#include <dwmapi.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace gfx {

namespace {

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool read_dword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD* out) {
    DWORD size = sizeof *out;
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, out, &size) == ERROR_SUCCESS;
}

constexpr wchar_t kPersonalize[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

const wchar_t* font_family() {
    static std::wstring family = [] {
        IDWriteFontCollection* fonts = nullptr;
        std::wstring pick = L"Segoe UI";
        if (SUCCEEDED(dwrite()->GetSystemFontCollection(&fonts, FALSE))) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            // Windows 11's UI font; Windows 10 falls back to Segoe UI.
            if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Text", &index, &exists)) && exists)
                pick = L"Segoe UI Variable Text";
            fonts->Release();
        }
        return pick;
    }();
    return family.c_str();
}

}  // namespace

ID2D1Factory* d2d() {
    static ID2D1Factory* factory = [] {
        ID2D1Factory* f = nullptr;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f);
        return f;
    }();
    return factory;
}

IDWriteFactory* dwrite() {
    static IDWriteFactory* factory = [] {
        IDWriteFactory* f = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&f));
        return f;
    }();
    return factory;
}

IWICImagingFactory* wic() {
    static IWICImagingFactory* factory = [] {
        IWICImagingFactory* f = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        return f;
    }();
    return factory;
}

D2D1_COLOR_F rgb(unsigned hex, float a) {
    return D2D1::ColorF(hex, a);
}

D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

Theme make_theme(bool dark, unsigned accent) {
    Theme t;
    t.dark = dark;
    t.bg = rgb(dark ? 0x1f1f1f : 0xf3f3f3);
    t.fg = rgb(dark ? 0xffffff : 0x1b1b1b);
    t.accent = rgb(accent);
    float lum = 0.2126f * t.accent.r + 0.7152f * t.accent.g + 0.0722f * t.accent.b;
    t.on_accent = lum > 0.55f ? rgb(0x000000) : rgb(0xffffff);
    t.raised = rgb(dark ? 0x434343 : 0xffffff);
    t.red = rgb(0xe01b24);
    t.blue = rgb(dark ? 0x62a0ea : 0x1c71d8);
    return t;
}

Theme system_theme() {
    DWORD light = 0;
    bool dark = !(read_dword(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme", &light) && light);
    // AccentPalette holds 8 RGBA swatches from lightest to darkest; Windows itself uses the second
    // (light 2) on dark backgrounds and the fifth (dark 1) on light ones.
    unsigned accent = dark ? 0x4cc2ff : 0x005fb8;
    BYTE palette[32];
    DWORD size = sizeof palette;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, palette, &size) == ERROR_SUCCESS &&
        size >= 32) {
        const BYTE* c = palette + (dark ? 1 : 4) * 4;
        accent = unsigned(c[0]) << 16 | unsigned(c[1]) << 8 | c[2];
    } else {
        DWORD color = 0;
        BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) accent = color & 0xFFFFFF;
    }
    return make_theme(dark, accent);
}

bool taskbar_is_light() {
    DWORD light = 0;
    return read_dword(HKEY_CURRENT_USER, kPersonalize, L"SystemUsesLightTheme", &light) && light;
}

IDWriteTextFormat* font(float size, DWRITE_FONT_WEIGHT weight) {
    static std::map<std::pair<int, int>, IDWriteTextFormat*> cache;
    auto key = std::make_pair(int(size * 100), int(weight));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    dwrite()->CreateTextFormat(font_family(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               size, L"en-us", &f);
    cache[key] = f;
    return f;
}

std::pair<D2D1_COLOR_F, D2D1_COLOR_F> level_colors(int level, bool charging) {
    if (charging) return {rgb(0x1c71d8), rgb(0x62a0ea)};
    if (level >= 50) return {rgb(0x26a269), rgb(0x57e389)};
    if (level >= 20) return {rgb(0xe5a50a), rgb(0xf6d32d)};
    return {rgb(0xc01c28), rgb(0xf66151)};
}

void draw_buds(ID2D1RenderTarget* rt, float x, float y, float size, ID2D1Brush* brush) {
    D2D1::Matrix3x2F old;
    rt->GetTransform(&old);
    const float s = size / 16.0f;
    const D2D1_MATRIX_3X2_F place = D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(x, y) * old;
    // Left bud, tilted -20° about (4.6, 6); right bud mirrored, +20° about (11.4, 6).
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(-20, D2D1::Point2F(4.6f, 6)) * place);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(4.4f, 5), 2.9f, 3.1f), brush);
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(6.4f, 3.3f, 8.4f, 5.9f), 1, 1), brush);
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2.6f, 6, 4.8f, 13.4f), 1.1f, 1.1f), brush);
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(20, D2D1::Point2F(11.4f, 6)) * place);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(11.6f, 5), 2.9f, 3.1f), brush);
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(7.6f, 3.3f, 9.6f, 5.9f), 1, 1), brush);
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(11.2f, 6, 13.4f, 13.4f), 1.1f, 1.1f), brush);
    rt->SetTransform(old);
}

HICON tray_icon(int size, const TrayBadge& badge) {
    IWICBitmap* bitmap = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    HICON icon = nullptr;
    if (FAILED(wic()->CreateBitmap(UINT(size), UINT(size), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
        return nullptr;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                              96, 96);
    if (SUCCEEDED(d2d()->CreateWicBitmapRenderTarget(bitmap, props, &rt)) &&
        SUCCEEDED(rt->CreateSolidColorBrush(rgb(0), &brush))) {
        const float px = float(size);
        const D2D1_COLOR_F ink = rgb(badge.light_taskbar ? 0x1b1b1b : 0xffffff);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (badge.connected && badge.show_level && badge.level >= 0) {
            // Lowest earbud as a number, with a level bar underneath.
            std::wstring text = std::to_wstring(badge.level);
            IDWriteTextLayout* layout = nullptr;
            float font_size = px * (text.size() > 2 ? 0.50f : 0.68f);
            dwrite()->CreateTextLayout(text.c_str(), UINT32(text.size()), font(font_size, DWRITE_FONT_WEIGHT_BOLD), px,
                                       px * 0.80f, &layout);
            if (layout) {
                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                brush->SetColor(ink);
                rt->DrawTextLayout(D2D1::Point2F(0, -px * 0.04f), layout, brush);
                layout->Release();
            }
            float bar_h = px < 20 ? 2.0f : px * 0.12f;
            float top = px - bar_h - (px < 20 ? 0.0f : px * 0.02f);
            D2D1_RECT_F track = D2D1::RectF(px * 0.08f, top, px * 0.92f, top + bar_h);
            brush->SetColor(alpha(ink, 0.30f));
            rt->FillRoundedRectangle(D2D1::RoundedRect(track, bar_h / 2, bar_h / 2), brush);
            auto [dark_tone, light_tone] = level_colors(badge.level, badge.charging);
            brush->SetColor(badge.light_taskbar ? dark_tone : light_tone);
            D2D1_RECT_F fill = track;
            fill.right = track.left + (track.right - track.left) * float(std::max(badge.level, 8)) / 100.0f;
            rt->FillRoundedRectangle(D2D1::RoundedRect(fill, bar_h / 2, bar_h / 2), brush);
        } else {
            brush->SetColor(alpha(ink, badge.connected ? 1.0f : 0.55f));
            draw_buds(rt, 0, 0, px, brush);
        }
        if (SUCCEEDED(rt->EndDraw())) {
            std::vector<BYTE> pixels(size_t(size) * size_t(size) * 4);
            if (SUCCEEDED(bitmap->CopyPixels(nullptr, UINT(size) * 4, UINT(pixels.size()), pixels.data()))) {
                BITMAPV5HEADER bi = {};
                bi.bV5Size = sizeof bi;
                bi.bV5Width = size;
                bi.bV5Height = -size;
                bi.bV5Planes = 1;
                bi.bV5BitCount = 32;
                bi.bV5Compression = BI_BITFIELDS;
                bi.bV5RedMask = 0x00FF0000;
                bi.bV5GreenMask = 0x0000FF00;
                bi.bV5BlueMask = 0x000000FF;
                bi.bV5AlphaMask = 0xFF000000;
                void* bits = nullptr;
                HDC dc = GetDC(nullptr);
                HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
                ReleaseDC(nullptr, dc);
                if (color && bits) {
                    // Icons take straight alpha; Direct2D drew premultiplied.
                    for (size_t i = 0; i < pixels.size(); i += 4) {
                        BYTE a = pixels[i + 3];
                        if (a && a < 255)
                            for (int c = 0; c < 3; c++) pixels[i + c] = BYTE(std::min(255, pixels[i + c] * 255 / a));
                    }
                    memcpy(bits, pixels.data(), pixels.size());
                    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
                    ICONINFO ii = {TRUE, 0, 0, mask, color};
                    icon = CreateIconIndirect(&ii);
                    DeleteObject(mask);
                }
                if (color) DeleteObject(color);
            }
        }
    }
    release(brush);
    release(rt);
    release(bitmap);
    return icon;
}

}  // namespace gfx
