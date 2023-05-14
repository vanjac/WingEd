#include "mathutil.h"
#include <glm/gtx/associated_min_max.hpp>

namespace winged {

int maxAxis(glm::vec3 v) {
    return glm::associatedMax(v.x, 0, v.z, 2, v.y, 1); // last value has priority!
}

glm::vec3 accumPolyNormal(glm::vec3 v1, glm::vec3 v2) {
    // Newell's method
    // https://web.archive.org/web/20070507025303/http://www.acm.org/tog/GraphicsGems/gemsiii/newell.c
    // an extension to 3D of https://stackoverflow.com/a/1165943
    glm::vec3 sum = v1 + v2;
    glm::vec3 diff = v1 - v2;
    return glm::vec3(diff.y * sum.z, diff.z * sum.x, diff.x * sum.y);
}

} // namespace
