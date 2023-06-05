#include "id.h"
#include <rpc.h>

namespace winged {

id_t genId() {
    id_t id;
    UuidCreate(&id);
    return id;
}

} // namespace
