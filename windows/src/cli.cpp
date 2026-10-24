// nordbuds.com: the command line front end. Typing "nordbuds status" in a terminal runs this
// (Windows tries .com before .exe); it passes the command to the running nordbuds.exe, starting it
// in the background first if needed, and prints the answer.
#include <windows.h>
#include <shellapi.h>

#include <cstring>
#include <string>
#include <vector>

#include "ipc.h"
#include "util.h"

namespace {

constexpr const char* kUsage = R"(Usage: nordbuds [--device AA:BB:CC:DD:EE:FF] [command]

With no command, opens the window. Nord Buds keeps running in the notification area.

Commands:
  --background, -b            start the tray icon only (used at sign-in)
  status                      everything the earbuds report
  battery                     left, right and case battery
  eq [balanced|bold|bass]     show or set the sound profile
  game [on|off]               show or set game mode (low latency)
  find [on|off]               ring the earbuds (default on); stops by itself after 60 s
  touch                       show touch controls
  touch <left|right|both> <single|double|triple|hold> <action>
                              actions: none, play-pause, previous, next,
                                       voice-assistant, game-mode, switch-device
  connect | disconnect        connect or disconnect Bluetooth audio
  tray [icon|battery]         show or set what the tray icon shows
  autostart [on|off]          show or set starting with Windows
  show                        open the window
  quit                        stop Nord Buds
)";

int exit_code = -1;
HWND service = nullptr;
ULONGLONG deadline = 0;

void write(DWORD which, const char* data, size_t n) {
    DWORD written = 0;
    WriteFile(GetStdHandle(which), data, DWORD(n), &written, nullptr);
}

void say(DWORD which, const std::string& s) {
    write(which, s.data(), s.size());
}

LRESULT CALLBACK reply_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (cds->dwData == ipc::kOut) write(STD_OUTPUT_HANDLE, static_cast<const char*>(cds->lpData), cds->cbData);
        else if (cds->dwData == ipc::kErr) write(STD_ERROR_HANDLE, static_cast<const char*>(cds->lpData), cds->cbData);
        else if (cds->dwData == ipc::kExit && cds->cbData >= sizeof(int)) {
            std::memcpy(&exit_code, cds->lpData, sizeof(int));
            PostQuitMessage(0);
        }
        return TRUE;
    }
    if (msg == WM_TIMER) {
        if (!IsWindow(service)) {
            say(STD_ERROR_HANDLE, "nordbuds: Nord Buds stopped before answering\n");
            exit_code = 1;
            PostQuitMessage(0);
        } else if (GetTickCount64() > deadline) {
            say(STD_ERROR_HANDLE, "nordbuds: no answer from Nord Buds\n");
            exit_code = 1;
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring gui_path() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    p.resize(p.find_last_of(L"\\/") + 1);
    return p + L"nordbuds.exe";
}

bool launch(const std::wstring& arguments) {
    std::wstring cmd = L"\"" + gui_path() + L"\" " + arguments;
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

HWND start_service(const std::string& device) {
    std::wstring arguments = L"--background";
    if (!device.empty()) arguments += L" --device " + util::wide(device);
    if (!launch(arguments)) return nullptr;
    for (int i = 0; i < 100; i++) {
        if (HWND h = ipc::find_service()) return h;
        Sleep(100);
    }
    return nullptr;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    std::string device;
    bool background = false;
    for (int i = 1; i < argc; i++) {
        std::string a = util::utf8(argv[i]);
        if (a == "--device" && i + 1 < argc) {
            device = util::utf8(argv[++i]);
        } else if (a == "--background" || a == "-b") {
            background = true;
        } else if (a == "--help" || a == "-h" || a == "help") {
            say(STD_OUTPUT_HANDLE, kUsage);
            return 0;
        } else {
            args.push_back(a);
        }
    }

    service = ipc::find_service();
    if (args.empty()) {
        if (background) {
            if (!service && !start_service(device)) {
                say(STD_ERROR_HANDLE, "nordbuds: could not start nordbuds.exe\n");
                return 1;
            }
            return 0;
        }
        std::wstring arguments = device.empty() ? L"" : L"--device " + util::wide(device);
        return launch(arguments) ? 0 : 1;
    }
    if (!service) {
        if (args[0] == "quit") {
            say(STD_OUTPUT_HANDLE, "Nord Buds is not running.\n");
            return 0;
        }
        service = start_service(device);
        if (!service) {
            say(STD_ERROR_HANDLE, "nordbuds: could not start nordbuds.exe\n");
            return 1;
        }
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = reply_proc;
    wc.hInstance = inst;
    wc.lpszClassName = ipc::kClientClass;
    RegisterClassExW(&wc);
    HWND reply = CreateWindowExW(0, ipc::kClientClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);

    DWORD pid = 0;
    GetWindowThreadProcessId(service, &pid);
    AllowSetForegroundWindow(pid);  // so "show" can bring the window to the front
    auto data = ipc::pack(reply, args);
    if (!ipc::send(service, ipc::kCommand, data.data(), DWORD(data.size()), reply)) {
        say(STD_ERROR_HANDLE, "nordbuds: Nord Buds is not answering\n");
        return 1;
    }
    deadline = GetTickCount64() + 90 * 1000;
    SetTimer(reply, 1, 500, nullptr);
    MSG msg;
    while (exit_code < 0 && GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    return exit_code < 0 ? 1 : exit_code;
}
