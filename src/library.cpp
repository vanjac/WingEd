#include "library.h"

namespace winged {

void Library::addFile(id_t id, std::wstring path) {
    idPaths[id] = path;
    pathIds[path] = id;
}

} // namespace
