#pragma once
#include "common.h"

#include <immer/set.hpp>
#include "surface.h"

namespace winged {

enum SelectMode {
    SEL_ELEMENTS, SEL_SOLIDS, NUM_SELMODES
};

// saved in file and undo stack
struct EditorState {
    Surface surf;
    immer::set<vert_id> selVerts;
    immer::set<face_id> selFaces;
    immer::set<edge_id> selEdges; // only primary edges!
    union {
        struct{} SAVE_DATA;
        SelectMode selMode = SEL_ELEMENTS;
    };
    bool gridOn = true;
    float gridSize = 0.25f;
};

// saved in file but not undo stack
struct ViewState {
    glm::vec3 camPivot = {};
    float rotX = 0, rotY = 0;
    float zoom = 4;
    bool flyCam = false;
};

} // namespace
