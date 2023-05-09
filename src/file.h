#pragma once
#include "common.h"

#include "editor.h"

namespace winged {

void writeFile(wchar_t *file, const EditorState &state);
EditorState readFile(wchar_t *file);

} // namespace
