#pragma once

#include <exception>

// win32
#define UNICODE
#define _UNICODE
#define WINVER          0x0400 // Windows 95
#define _WIN32_WINNT    0x0400 // Windows NT 4.0

#pragma warning(disable: 4201) // for glm

struct winged_error : std::exception {
    const wchar_t *message = nullptr;
    winged_error() = default;
    winged_error(const wchar_t *message) : message(message) {}
};
