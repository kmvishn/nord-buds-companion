#include "tray.h"

#include <shellapi.h>
#include <windowsx.h>

#include <cwchar>

#include "buds.h"
#include "gfx.h"
#include "model.h"
#include "settings.h"
#include "util.h"

namespace {

constexpr wchar_t kClass[] = L"NordBudsTray";
constexpr UINT kCallback = WM_APP + 1;
constexpr UINT kIconId = 1;
enum : UINT { kOpen = 1, kConnect, kDisconnect, kQuit, kGame, kFind, kEqBase = 100 };

std::wstring percent(const proto::BatteryPart& b) {
    if (b.level < 0) return L"–";
    return std::to_wstring(b.level) + L"%" + (b.charging ? L" (charging)" : L"");
}

std::wstring status_line(const Buds& buds) {
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

NOTIFYICONDATAW base_data(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = kIconId;
    return nid;
}

}  // namespace

Tray::Tray(Buds& buds, Actions actions) : buds_(buds), actions_(std::move(actions)) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    // A hidden top-level window: message-only windows do not get TaskbarCreated or theme broadcasts.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"Nord Buds", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    add();
    update();
}

Tray::~Tray() {
    NOTIFYICONDATAW nid = base_data(hwnd_);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (icon_) DestroyIcon(icon_);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    DestroyWindow(hwnd_);
}

void Tray::add() {
    NOTIFYICONDATAW nid = base_data(hwnd_);
    nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kCallback;
    wcscpy_s(nid.szTip, L"Nord Buds");
    added_ = Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    icon_key_.clear();
    tip_.clear();
}

void Tray::update() {
    if (!added_) add();
    const auto& st = buds_.state();
    bool ready = buds_.ready();

    gfx::TrayBadge badge;
    badge.connected = ready;
    badge.show_level = Settings::get().tray_battery();
    badge.light_taskbar = gfx::taskbar_is_light();
    if (ready) {
        for (const proto::BatteryPart* b : {&st.battery.left, &st.battery.right}) {
            if (b->level >= 0 && (badge.level < 0 || b->level < badge.level)) {
                badge.level = b->level;
                badge.charging = b->charging;
            }
        }
    }
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    std::wstring key = std::to_wstring(size) + L"/" + std::to_wstring(badge.connected) + std::to_wstring(badge.show_level) +
                       L"/" + std::to_wstring(badge.level) + std::to_wstring(badge.charging) +
                       std::to_wstring(badge.light_taskbar);

    std::wstring tip = L"Nord Buds · " + status_line(buds_);
    if (ready)
        tip += L"\nLeft " + percent(st.battery.left) + L" · Right " + percent(st.battery.right) + L" · Case " +
               percent(st.battery.box);

    if (key == icon_key_ && tip == tip_) return;
    NOTIFYICONDATAW nid = base_data(hwnd_);
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    HICON old = nullptr;
    if (key != icon_key_) {
        if (HICON icon = gfx::tray_icon(size, badge)) {
            old = icon_;
            icon_ = icon;
            nid.uFlags |= NIF_ICON;
            nid.hIcon = icon_;
            icon_key_ = key;
        }
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (old) DestroyIcon(old);
    tip_ = tip;
}

void Tray::notify(const std::wstring& title, const std::wstring& body) {
    NOTIFYICONDATAW nid = base_data(hwnd_);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, body.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::clear_notification() {
    NOTIFYICONDATAW nid = base_data(hwnd_);
    nid.uFlags = NIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::show_menu(int x, int y) {
    const auto& st = buds_.state();
    bool ready = buds_.ready();
    HMENU m = CreatePopupMenu();
    if (ready) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"Left: " + percent(st.battery.left)).c_str());
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"Right: " + percent(st.battery.right)).c_str());
        std::wstring box = st.battery.box.level < 0 ? L"– (open the lid)" : percent(st.battery.box);
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"Case: " + box).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        if (buds_.supports(proto::kEqSet)) {
            UINT first = 0, last = 0, checked = 0;
            for (const auto& preset : model::eq_presets()) {
                UINT id = kEqBase + preset.id;
                AppendMenuW(m, MF_STRING, id, util::wide(preset.name).c_str());
                if (!first) first = id;
                first = std::min(first, id);
                last = std::max(last, id);
                if (st.eq == preset.id) checked = id;
            }
            if (checked) CheckMenuRadioItem(m, first, last, checked, MF_BYCOMMAND);
        }
        if (buds_.supports(proto::kFeatureSet) && st.game_mode >= 0)
            AppendMenuW(m, MF_STRING | (st.game_mode == 1 ? MF_CHECKED : 0), kGame, L"Game mode");
        if (buds_.supports(proto::kFind))
            AppendMenuW(m, MF_STRING, kFind, st.finding ? L"Stop ringing" : L"Ring earbuds");
    } else {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, status_line(buds_).c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kOpen, L"Open Nord Buds");
    SetMenuDefaultItem(m, kOpen, FALSE);
    if (st.link != LinkState::NoDevice) {
        if (st.bt_connected && !st.disconnecting) AppendMenuW(m, MF_STRING, kDisconnect, L"Disconnect");
        else AppendMenuW(m, MF_STRING, kConnect, L"Connect");
    }
    AppendMenuW(m, MF_STRING, kQuit, L"Quit");

    // Required for the menu to close when the user clicks elsewhere.
    SetForegroundWindow(hwnd_);
    UINT choice = UINT(TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN |
                                               (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN),
                                        x, y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);

    if (choice == kOpen && actions_.show) actions_.show();
    else if (choice == kConnect) buds_.bluetooth_connect();
    else if (choice == kDisconnect) buds_.bluetooth_disconnect();
    else if (choice == kQuit && actions_.quit) actions_.quit();
    else if (choice == kGame) buds_.set_game_mode(buds_.state().game_mode != 1);
    else if (choice == kFind) buds_.set_find(!buds_.state().finding);
    else if (choice >= kEqBase) buds_.set_eq(int(choice - kEqBase));
}

LRESULT CALLBACK Tray::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == self->taskbar_created_) {
        // Explorer restarted: the icon is gone and must be added again.
        self->added_ = false;
        self->update();
        return 0;
    }
    switch (msg) {
    case kCallback:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (self->actions_.show) self->actions_.show();
            break;
        case WM_CONTEXTMENU:
            self->show_menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        default:
            break;
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            self->icon_key_.clear();
            self->update();
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}
