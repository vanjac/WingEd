#pragma once
#include "common.h"

#include "surface.h"

namespace winged {

struct EditorState {
    Surface surf;
    union {
        id_t sel = {};
        vert_id selVert;
        face_id selFace;
        edge_id selEdge;
    };
    bool gridOn = true;
    float gridSize = 0.25f;
};

} // namespace
