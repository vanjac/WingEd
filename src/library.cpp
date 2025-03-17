#include "library.h"
#include "strutil.h"

namespace winged {

void Library::clear() {
    idPaths.clear();
    pathIds.clear();
}

void Library::addFile(id_t id, std::string path) {
    idPaths[id] = path;
    pathIds[path] = id;
}

} // namespace
