// Reading/writing various file formats

#pragma once
#include "common.h"

#include "editor.h"
#include "library.h"

namespace winged {

void writeFile(const wchar_t *file, const EditorState &state, const ViewState &view,
    const Library &library);
std::tuple<EditorState, ViewState> readFile(const wchar_t *file);

void writeObj(const wchar_t *file, const Surface &surf);

} // namespace
