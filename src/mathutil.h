#pragma once
#include "common.h"

#include <glm/vec3.hpp>

namespace winged {

struct Ray {
    glm::vec3 org, dir;
};

int maxAxis(glm::vec3 v);
glm::vec3 accumPolyNormal(glm::vec3 v1, glm::vec3 v2); // single step of calculating polygon normal

} // namespace
