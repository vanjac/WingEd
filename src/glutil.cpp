#include "glutil.h"
#include "winchroma.h" // required for GLU
#include <gl/GLU.h>

namespace winged {

void texImageMipmaps(unsigned int target, int internalFormat, int width, int height,
        unsigned int format, unsigned int type, void *data) {
    gluBuild2DMipmaps(target, internalFormat, width, height, format, type, data);
}

} // namespace
