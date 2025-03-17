#pragma once
#include "common.h"

#include <string>
#include <unordered_map>
#include "id.h"

namespace winged {

struct Library {
    std::string rootPath; // if empty, use containing directory of file when saved

    // external file references (absolute paths)
    std::unordered_map<id_t, std::string> idPaths;
    std::unordered_map<std::string, id_t> pathIds;

    void clear();
    void addFile(id_t id, std::string path);
};

} // namespace
