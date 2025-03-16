#include "strutil.h"
#include "winchroma.h"
#include <memory>
#include <stringapiset.h>
#include <winnls.h>

namespace winged {

std::string narrow(const wchar_t *s) {
    auto bufSize = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (bufSize <= 0) {
        return "";
    }
    auto buf = std::make_unique<char[]>(bufSize);
    CHECKERR(WideCharToMultiByte(CP_UTF8, 0, s, -1, buf.get(), bufSize, NULL, NULL));
    return buf.get();
}

std::wstring widen(const char *s) {
    auto bufSize = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (bufSize <= 0) {
        return L"";
    }
    auto buf = std::make_unique<wchar_t[]>(bufSize);
    CHECKERR(MultiByteToWideChar(CP_UTF8, 0, s, -1, buf.get(), bufSize));
    return buf.get();
}

std::string narrow(const std::wstring &s) {
    return narrow(s.c_str());
}

std::wstring widen(const std::string &s) {
    return widen(s.c_str());
}

} // namespace

#ifdef ENTRY_TEST_STRUTIL
#include <cstdio>
using namespace winged;

int main() {
    wprintf(L"Narrow: ==%S==\n", narrow(L"Test string").c_str());
    wprintf(L"Widen: ==%s==\n", widen("Test string").c_str());
}
#endif // ENTRY_TEST_STRUTIL
