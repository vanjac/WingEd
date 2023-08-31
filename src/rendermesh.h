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
    ELEM_ERR_FACE,
    ELEM_DRAW_POINT, ELEM_DRAW_LINE,
    ELEM_COUNT
};

struct IndexRange {
    size_t start, count;
};

struct RenderFaceMesh {
    id_t material;
    IndexRange range;
    enum State {REG, SEL, HOV} state;
};

struct RenderMesh {
    std::vector<glm::vec3> vertices, normals;
    std::vector<glm::vec2> texCoords;
    std::vector<index_t> indices; // indices into above vectors
    IndexRange ranges[ELEM_COUNT];
    std::vector<RenderFaceMesh> faceMeshes;

    void clear();
};

void initRenderMesh();
void generateRenderMesh(RenderMesh *mesh, const EditorState &state);
void tesselateFace(std::vector<index_t> &faceIsOut, const Surface &surf,
    const Face &face, glm::vec3 normal);

} // namespace
