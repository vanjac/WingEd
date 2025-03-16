// https://utf8everywhere.org/#windows

#pragma once
#include "common.h"

#include <string>

namespace winged {

std::string narrow(const wchar_t *s);
std::wstring widen(const char *s);
std::string narrow(const std::wstring &s);
std::wstring widen(const std::string &s);

} // namespace
