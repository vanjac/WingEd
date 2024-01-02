// General math utility functions / types

#pragma once
#include "common.h"

#include <optional>
#include <glm/vec3.hpp>

namespace winged {

struct Ray {
    glm::vec3 org, dir;
};
struct Plane {
    glm::vec3 org, norm;
};

int maxAxis(glm::vec3 v);
glm::vec3 accumPolyNormal(glm::vec3 v1, glm::vec3 v2); // single step of calculating polygon normal
std::optional<glm::vec3> intersectRayPlane(const Ray &ray, const Plane &plane);
float solvePlane(glm::vec3 vec, glm::vec3 norm, int axis);

inline float fixZero(float f) { return (f == 0) ? 0 : f; } // fix negative zero
#define VEC3_ARGS(v) fixZero(v.x), fixZero(v.y), fixZero(v.z)

} // namespace
