// Reading/writing various file formats

#pragma once
#include "common.h"

#include "editor.h"
#include "library.h"

#include <string>

namespace winged {

void writeFile(const std::string &file, const EditorState &state, const ViewState &view,
    const Library &library);
std::tuple<EditorState, ViewState, Library> readFile(const std::string &file,
    const std::string &libraryPath);

void writeObj(const std::string &file, const Surface &surf, const Library &library,
    const std::string &mtlName, bool writeMtl);

} // namespace
