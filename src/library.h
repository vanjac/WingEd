#pragma once
#include "common.h"

#include <tchar.h>
#include <string>
#include <unordered_map>
#include "id.h"

namespace winged {

struct Library {
    std::wstring rootPath; // if empty, use containing directory of file when saved

    // external file references (absolute paths)
    std::unordered_map<id_t, std::wstring> idPaths;
    std::unordered_map<std::wstring, id_t> pathIds;

    void addFile(id_t id, std::wstring path);
};

} // namespace
