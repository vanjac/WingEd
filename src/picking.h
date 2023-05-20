#pragma once
#include "common.h"

#include "surface.h"
#include "mathutil.h"
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

namespace winged {

using PickType = uint32_t;
const PickType
    PICK_NONE = 0x0,
    PICK_VERT = 0x1,
    PICK_FACE = 0x2,
    PICK_EDGE = 0x4,
    PICK_ELEMENT = PICK_VERT | PICK_FACE | PICK_EDGE;

struct PickResult {
    PickType type = PICK_NONE;
    union {
        id_t id = {};
        vert_id vert;
        face_id face;
        edge_id edge;
        size_t val; // generic data
    };
    glm::vec3 point = {};
    float depth = 2; // NDC, range -1 to 1
    PickResult() = default;
    PickResult(PickType type, id_t id, glm::vec3 point, float depth)
        : type(type), id(id), point(point), depth(depth) {}
};

glm::vec2 screenPosToNDC(glm::vec2 pos, glm::vec2 windowDim);
Ray viewPosToRay(glm::vec2 normPos, const glm::mat4 &project);

glm::vec3 snapPlanePoint(glm::vec3 point, const Plane &plane, float grid);

bool pickVert(glm::vec3 vertPos, glm::vec2 normCur, glm::vec2 windowDim, const glm::mat4 &project,
    float *depth);

PickResult pickElement(const Surface &surf, PickType types,
    glm::vec2 normCur, glm::vec2 windowDim, const glm::mat4 &project,
    float grid = 0, float maxDepth = 2);

} // namespace
