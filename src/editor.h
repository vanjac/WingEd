#pragma once
#include "common.h"

#include <immer/set.hpp>
#include "surface.h"

namespace winged {

struct EditorState {
    Surface surf;
    immer::set<vert_id> selVerts;
    immer::set<face_id> selFaces;
    immer::set<edge_id> selEdges;
    union {
        struct{} START_DATA;
        bool gridOn = true;
    };
    float gridSize = 0.25f;
};

} // namespace
