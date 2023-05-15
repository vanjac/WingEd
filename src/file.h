#pragma once
#include "common.h"

#include "editor.h"

namespace winged {

void writeFile(wchar_t *file, const EditorState &state, const ViewState &view);
std::tuple<EditorState, ViewState> readFile(wchar_t *file);

} // namespace
