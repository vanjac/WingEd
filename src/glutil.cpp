#include "glutil.h"
#include "winchroma.h" // required for GLU
#include <GL/glu.h>

namespace winged {

void texImageMipmaps(unsigned int target, int internalFormat, int width, int height,
        unsigned int format, unsigned int type, void *data) {
    // TODO use either GL_GENERATE_MIPMAP or glGenerateMipmap, + confirm power of two sizing
    // https://www.khronos.org/opengl/wiki/Common_Mistakes#gluBuild2DMipmaps
    // https://www.gamedev.net/forums/topic/452780-gl_generate_mipmap-vs-glubuild2dmipmaps/
    gluBuild2DMipmaps(target, internalFormat, width, height, format, type, data);
}

} // namespace
