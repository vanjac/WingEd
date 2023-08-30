// OpenGL utility functions

#pragma once
#include "common.h"

namespace winged {

void texImageMipmaps(unsigned int target, int internalFormat, int width, int height,
    unsigned int format, unsigned int type, void *data);

} // namespace
