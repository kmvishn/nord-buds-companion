#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define NORDBUDS_TEST_HOOKS

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <iostream>
#include <memory>
#include <vector>

#include "../windows/src/buds.h"
#include "../windows/src/window.h"
#include "../windows/src/gfx.h"
#include "../windows/src/model.h"

bool save_wic_to_png(IWICBitmap* bitmap, const wchar_t* path) {
    IWICStream* stream = nullptr;
    if (FAILED(gfx::wic()->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) {
        stream->Release();
        return false;
    }

    IWICBitmapEncoder* encoder = nullptr;
    if (FAILED(gfx::wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
        stream->Release();
        return false;
    }
    encoder->Initialize(stream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode* frame = nullptr;
    encoder->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);

    UINT width = 0, height = 0;
    bitmap->GetSize(&width, &height);
    frame->SetSize(width, height);

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    frame->SetPixelFormat(&format);
    frame->WriteSource(bitmap, nullptr);

    frame->Commit();
    encoder->Commit();

    frame->Release();
    encoder->Release();
    stream->Release();
    return true;
}

void render_view(MainWindow& win, Buds& buds, bool dark, const wchar_t* output_path) {
    float width_dip = 440.0f;
    float height_dip = win.content_height(width_dip) + 20.0f;
    float scale = 2.0f; // 2x Retina / HiDPI for sharp rendering
    UINT width_px = static_cast<UINT>(width_dip * scale);
    UINT height_px = static_cast<UINT>(height_dip * scale);

    IWICBitmap* bitmap = nullptr;
    if (FAILED(gfx::wic()->CreateBitmap(width_px, height_px, GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapCacheOnLoad, &bitmap))) {
        std::cerr << "Failed to create WIC bitmap" << std::endl;
        return;
    }

    auto rt_props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f * scale, 96.0f * scale
    );

    ID2D1RenderTarget* rt = nullptr;
    if (FAILED(gfx::d2d()->CreateWicBitmapRenderTarget(bitmap, rt_props, &rt))) {
        std::cerr << "Failed to create D2D render target" << std::endl;
        bitmap->Release();
        return;
    }

    // Set theme
    win.set_theme(gfx::make_theme(dark, dark ? 0x4cc2ff : 0x005fb8));
    win.refresh();
    win.finish_animations();

    rt->BeginDraw();
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    win.paint(rt, width_dip, height_dip);

    if (SUCCEEDED(rt->EndDraw())) {
        if (save_wic_to_png(bitmap, output_path)) {
            std::wcout << L"Successfully saved: " << output_path << std::endl;
        } else {
            std::wcerr << L"Failed to save PNG: " << output_path << std::endl;
        }
    } else {
        std::cerr << "EndDraw failed" << std::endl;
    }

    rt->Release();
    bitmap->Release();
}

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Buds buds("");
    BudsState st;
    st.link = LinkState::Connected;
    st.name = "OnePlus Nord Buds 2r";
    st.address = "78:2B:64:12:89:C4";
    st.radio = true;
    st.bt_connected = true;
    st.initialized = true;
    st.battery.left = {85, false};
    st.battery.right = {90, false};
    st.battery.box = {65, true}; // Case charging
    st.eq = 0; // Balanced
    st.game_mode = 0; // Off
    st.firmware = {"113", "113", "101", "113.113.101"};
    st.bitmap.assign(32, 0xFF); // Support everything

    // Gesture table (OnePlus Nord Buds 2r factory defaults)
    st.gestures = {
        {1, 1, 1, 0x01}, // Left Single: Play/pause
        {1, 1, 2, 0x06}, // Left Double: Next track
        {1, 1, 3, 0x05}, // Left Triple: Previous track
        {1, 1, 4, 0x0D}, // Left Hold: Switch device
        {2, 1, 1, 0x01}, // Right Single: Play/pause
        {2, 1, 2, 0x06}, // Right Double: Next track
        {2, 1, 3, 0x05}, // Right Triple: Previous track
        {2, 1, 4, 0x0D}, // Right Hold: Switch device
    };

    buds.set_test_state(st);

    MainWindow win(buds, MainWindow::Actions{});

    render_view(win, buds, true, L"docs/screenshots/windows-connected-dark.png");
    render_view(win, buds, false, L"docs/screenshots/windows-connected-light.png");

    CoUninitialize();
    return 0;
}
