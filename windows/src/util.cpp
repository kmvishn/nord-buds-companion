#include "util.h"

#include <cstdio>

namespace util {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string error_text(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string text;
    if (n && buf) {
        text = utf8(std::wstring(buf, n));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.' || text.back() == ' '))
            text.pop_back();
    }
    if (buf) LocalFree(buf);
    if (text.empty()) {
        char hex[32];
        std::snprintf(hex, sizeof hex, "error 0x%08lX", static_cast<unsigned long>(code));
        text = hex;
    }
    return text;
}

bool run_detached(std::function<void()> fn) {
    auto* job = new std::function<void()>(std::move(fn));
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](void* data) -> DWORD {
            auto* f = static_cast<std::function<void()>*>(data);
            (*f)();
            delete f;
            return 0;
        },
        job, 0, nullptr);
    if (!thread) {
        delete job;
        return false;
    }
    CloseHandle(thread);
    return true;
}

}  // namespace util
