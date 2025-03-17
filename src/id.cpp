#include "id.h"
#include <rpc.h>

namespace winged {

id_t genId() {
    id_t id;
    UuidCreate(&id);
    return id;
}

static void printId(const id_t &id) {
    wprintf(L"{%08lX-%04hX-%04hX-%02hhX%02hhX-",
        id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1]);
    for (auto i = 2; i < 8; i++) {
        wprintf(L"%02hhX", id.Data4[i]);
    }
    wprintf(L"}\n");
}

} // namespace

#ifdef ENTRY_TEST_ID
#include <cstdio>
using namespace winged;

int main() {
    auto id = genId();
    wprintf(L"Generated ID: ");
    printId(id);
}
#endif // ENTRY_TEST_ID
