#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace util {

std::wstring wide(const std::string& s);
std::string utf8(const std::wstring& s);

// System message for a Win32 / Winsock / HRESULT error code, without the trailing newline.
std::string error_text(DWORD code);

// Runs fn on a new thread that nobody waits for. Returns false if the thread could not start.
bool run_detached(std::function<void()> fn);

}  // namespace util
