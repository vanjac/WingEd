#pragma once
#include "common.h"

#include "surface.h"

namespace winged {

void writeFile(wchar_t *file, const Surface &surf);
Surface readFile(wchar_t *file);

} // namespace
