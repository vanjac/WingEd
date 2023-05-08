#pragma once
#include "common.h"

#include "surface.h"
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

namespace winged {

const float PICK_POINT_SIZE = 9;
const float PICK_EDGE_SIZE = 7;

struct PickResult {
    Surface::ElementType type = Surface::NONE;
    union {
        id_t id = {};
        vert_id vert;
        face_id face;
        edge_id edge;
    };
    glm::vec3 point = {};
    PickResult() = default;
    PickResult(Surface::ElementType type, id_t id, glm::vec3 point)
        : type(type), id(id), point(point) {}
};

PickResult pickElement(const Surface &surf, Surface::ElementType types,
    glm::vec2 cursor, glm::vec2 windowDim, const glm::mat4 &project);

} // namespace
