#include "mathutil.h"
#include <glm/gtx/associated_min_max.hpp>
#include <glm/gtx/intersect.hpp>

namespace winged {

int maxAxis(glm::vec3 v) {
    return glm::associatedMax(v.x, 0, v.z, 2, v.y, 1); // last value has priority!
}

glm::vec3 accumPolyNormal(glm::vec3 v1, glm::vec3 v2) {
    // Newell's method
    // https://web.archive.org/web/20070507025303/http://www.acm.org/tog/GraphicsGems/gemsiii/newell.c
    // an extension to 3D of https://stackoverflow.com/a/1165943
    auto sum = v1 + v2;
    auto diff = v1 - v2;
    return glm::vec3(diff.y * sum.z, diff.z * sum.x, diff.x * sum.y);
}

std::optional<glm::vec3> intersectRayPlane(const Ray &ray, const Plane &plane) {
    float t;
    if (glm::intersectRayPlane(ray.org, ray.dir, plane.org, plane.norm, t)) {
        glm::vec3 point = ray.org + t * ray.dir;
        // fix precision issues, make sure point lies exactly on plane
        auto axis = maxAxis(glm::abs(plane.norm));
        point[axis] = plane.org[axis] + solvePlane(point - plane.org, plane.norm, axis);
        return point;
    }
    return std::nullopt;
}

float solvePlane(glm::vec3 vec, glm::vec3 norm, int axis) {
    auto a = (axis + 1) % 3, b = (axis + 2) % 3;
    return -(norm[a] * vec[a] + norm[b] * vec[b]) / norm[axis];
}

} // namespace
