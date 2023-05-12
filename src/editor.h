#pragma once
#include "common.h"

#include <immer/set.hpp>
#include "surface.h"

namespace winged {

enum SelectMode {
    SEL_ELEMENTS, SEL_SOLIDS, NUM_SELMODES
};

struct EditorState {
    Surface surf;
    immer::set<vert_id> selVerts;
    immer::set<face_id> selFaces;
    immer::set<edge_id> selEdges;
    union {
        struct{} START_DATA;
        SelectMode selMode = SEL_ELEMENTS;
    };
    bool gridOn = true;
    float gridSize = 0.25f;
};

} // namespace
