// The RenderMesh contains all data needed for rendering the current model. The same RenderMesh can
// be rendered from multiple camera angles, and with multiple view states.

#pragma once
#include "common.h"

#include "editor.h"
#include <vector>

namespace winged {

using index_t = unsigned short; // GLushort

struct RenderMesh {
    std::vector<glm::vec3> vertices, normals;
    // indices into above vectors:
    std::vector<index_t> regVertIs, selVertIs, hovVertIs; // regular, selected, hover
    std::vector<index_t> regEdgeIs, selEdgeIs, hovEdgeIs;
    std::vector<index_t> regFaceIs, selFaceIs, hovFaceIs, errFaceIs;
    std::vector<index_t> drawPointIs, drawLineIs;

    void clear();
};

void initRenderMesh();
void generateRenderMesh(RenderMesh *mesh, const EditorState &state);

} // namespace
