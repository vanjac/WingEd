// The RenderMesh contains all data needed for rendering the current model. The same RenderMesh can
// be rendered from multiple camera angles, and with multiple view states.

#pragma once
#include "common.h"

#include "editor.h"
#include <vector>

namespace winged {

using index_t = unsigned short; // GLushort

enum RenderElement {
    ELEM_REG_VERT, ELEM_SEL_VERT, ELEM_HOV_VERT, // regular, selected, hover
    ELEM_REG_EDGE, ELEM_SEL_EDGE, ELEM_HOV_EDGE,
    ELEM_REG_FACE, ELEM_SEL_FACE, ELEM_HOV_FACE, ELEM_ERR_FACE,
    ELEM_DRAW_POINT, ELEM_DRAW_LINE,
    ELEM_COUNT
};

struct RenderMesh {
    std::vector<glm::vec3> vertices, normals;
    std::vector<glm::vec2> texCoords;
    // indices into above vectors:
    std::vector<index_t> indices[ELEM_COUNT];

    void clear();
};

void initRenderMesh();
void generateRenderMesh(RenderMesh *mesh, const EditorState &state);

} // namespace
