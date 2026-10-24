#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Talking to the running Nord Buds over WM_COPYDATA. The command line tool (nordbuds.com) sends a
// command to the service window; the service answers with text and finally an exit code, sent to
// the tool's own message-only window.
namespace ipc {

constexpr wchar_t kServiceClass[] = L"NordBudsService";
constexpr wchar_t kClientClass[] = L"NordBudsClient";
constexpr wchar_t kMutex[] = L"Local\\NordBudsCompanion";

enum : ULONG_PTR {
    kCommand = 0x4E420001,  // 8-byte reply HWND, then NUL-separated UTF-8 arguments
    kOut = 0x4E420002,      // UTF-8 text for stdout
    kErr = 0x4E420003,      // UTF-8 text for stderr
    kExit = 0x4E420004,     // int exit code; the command is finished
};

inline HWND find_service() {
    return FindWindowExW(HWND_MESSAGE, nullptr, kServiceClass, nullptr);
}

inline std::vector<char> pack(HWND reply, const std::vector<std::string>& args) {
    std::vector<char> out(8, 0);
    uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(reply));
    std::memcpy(out.data(), &h, 8);
    for (const auto& a : args) {
        out.insert(out.end(), a.begin(), a.end());
        out.push_back('\0');
    }
    return out;
}

inline bool unpack(const COPYDATASTRUCT* cds, HWND* reply, std::vector<std::string>* args) {
    if (cds->dwData != kCommand || cds->cbData < 8) return false;
    const char* p = static_cast<const char*>(cds->lpData);
    uint64_t h = 0;
    std::memcpy(&h, p, 8);
    *reply = reinterpret_cast<HWND>(uintptr_t(h));
    args->clear();
    std::string cur;
    for (DWORD i = 8; i < cds->cbData; i++) {
        if (p[i] == '\0') {
            args->push_back(cur);
            cur.clear();
        } else {
            cur.push_back(p[i]);
        }
    }
    return true;
}

inline bool send(HWND to, ULONG_PTR kind, const void* data, DWORD size, HWND from = nullptr) {
    COPYDATASTRUCT cds = {kind, size, const_cast<void*>(data)};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(to, WM_COPYDATA, reinterpret_cast<WPARAM>(from), reinterpret_cast<LPARAM>(&cds),
                               SMTO_ABORTIFHUNG, 5000, &result) != 0;
}

}  // namespace ipc
