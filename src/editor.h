#pragma once
#include "common.h"

#include <immer/set.hpp>
#include <glm/trigonometric.hpp>
#include "surface.h"

namespace winged {

enum SelectMode {
    SEL_ELEMENTS, SEL_SOLIDS, NUM_SELMODES
};

enum ViewMode {
    VIEW_ORBIT, VIEW_FLY, VIEW_ORTHO, NUM_VIEWMODES
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
    float gridSize = 1;
    Plane workPlane = {{}, {0, 1, 0}};
    char reserved[4];
};

// per-viewport, saved in file but not undo stack
struct ViewState {
    glm::vec3 camPivot = {}; // TODO: actually negative
    float rotX = glm::radians(45.0f), rotY = glm::radians(-15.0f);
    float zoom = 16;
    ViewMode mode = VIEW_ORBIT;
};

bool hasSelection(EditorState state);
immer::set<vert_id> selAttachedVerts(const EditorState &state);
EditorState clearSelection(EditorState state);
EditorState cleanSelection(const EditorState &state);

glm::vec3 vertsCenter(const Surface &surf, immer::set<vert_id> verts);

} // namespace
