// Common definitions included by every other header

#pragma once

#include <exception>

#include <tuple>
using std::tie;
using std::tuple;

// win32
#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define WINVER          0x0501 // Windows XP
#define _WIN32_WINNT    0x0501

#pragma warning(disable: 4996) // :3

namespace winged {
struct winged_error : std::exception {
    const wchar_t *message = nullptr;
    winged_error() = default;
    winged_error(const wchar_t *message) : message(message) {}
};
}
