#include "mathutil.h"
#include <glm/gtx/associated_min_max.hpp>

namespace winged {

int maxAxis(glm::vec3 v) {
    return glm::associatedMax(v.x, 0, v.y, 1, v.z, 2);
}

} // namespace
