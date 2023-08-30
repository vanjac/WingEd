// Reading/writing various file formats

#pragma once
#include "common.h"

#include "editor.h"

namespace winged {

void writeFile(const wchar_t *file, const EditorState &state, const ViewState &view);
std::tuple<EditorState, ViewState> readFile(const wchar_t *file);

void writeObj(const wchar_t *file, const Surface &surf);

} // namespace
