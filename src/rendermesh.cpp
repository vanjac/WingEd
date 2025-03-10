#include "rendermesh.h"
#include <memory>
#include <unordered_map>
#include "winchroma.h" // required for GLU
#include <GL/glu.h>
#include <glm/gtc/type_ptr.hpp>
#include "main.h"

namespace winged {

struct FaceTessState {
    std::vector<index_t> *indices;
    GLenum mode;
    size_t startI;
    GLenum error = 0;
};

static GLUtesselator *g_tess;

static void CALLBACK tessBeginCallback(GLenum mode, void *data) {
    auto state = reinterpret_cast<FaceTessState *>(data);
    state->mode = mode;
    state->startI = state->indices->size();
}

static void CALLBACK tessVertexCallback(void *vertex, void *data) {
    auto state = reinterpret_cast<FaceTessState *>(data);
    auto index = index_t(size_t(vertex));
    auto numIndices = state->indices->size();
    if (state->mode == GL_TRIANGLE_STRIP && numIndices - state->startI >= 3) {
        if ((numIndices - state->startI) % 6 == 0) {
            state->indices->push_back((*state->indices)[numIndices - 3]);
            state->indices->push_back((*state->indices)[numIndices - 1]);
        } else {
            state->indices->push_back((*state->indices)[numIndices - 1]);
            state->indices->push_back((*state->indices)[numIndices - 2]);
        }
    } else if (state->mode == GL_TRIANGLE_FAN && numIndices - state->startI >= 3) {
        state->indices->push_back((*state->indices)[state->startI]);
        state->indices->push_back((*state->indices)[numIndices - 1]);
    }
    state->indices->push_back(index);
}

static void CALLBACK tessErrorCallback(GLenum error, void *data) {
    reinterpret_cast<FaceTessState *>(data)->error = error;
}

bool tesselateFace(std::vector<index_t> &faceIsOut, const Surface &surf, const Face &face,
        glm::vec3 normal, index_t vertI) {
    // https://www.glprogramming.com/red/chapter11.html
    auto initialSize = faceIsOut.size();
    FaceTessState state;
    state.indices = &faceIsOut;
    gluTessNormal(g_tess, normal.x, normal.y, normal.z);
    gluTessBeginPolygon(g_tess, &state);
    gluTessBeginContour(g_tess);
    for (auto ep : FaceEdges(surf, face)) {
        glm::dvec3 dPos = ep.second.vert.in(surf).pos;
        gluTessVertex(g_tess, glm::value_ptr(dPos), void_p(size_t(vertI++)));
    }
    gluTessEndContour(g_tess);
    gluTessEndPolygon(g_tess);

    if (state.error)
        faceIsOut.erase(faceIsOut.begin() + initialSize, faceIsOut.end());
    return !state.error;
}

void initRenderMesh() {
    g_tess = gluNewTess();
    using callback_fn_t = GLvoid (CALLBACK*) ();
    gluTessCallback(g_tess, GLU_TESS_BEGIN_DATA, callback_fn_t(tessBeginCallback));
    // gluTessCallback(g_tess, GLU_TESS_END, callback_fn_t(tessEndCallback));
    gluTessCallback(g_tess, GLU_TESS_VERTEX_DATA, callback_fn_t(tessVertexCallback));
    gluTessCallback(g_tess, GLU_TESS_ERROR_DATA, callback_fn_t(tessErrorCallback));
    // gluTessCallback(g_tess, GLU_TESS_COMBINE_DATA, callback_fn_t(tessCombineCallback));
    // gluTessCallback(g_tess, GLU_TESS_EDGE_FLAG_DATA, callback_fn_t(tessEdgeFlagCallback));
}

void RenderMesh::clear() {
    vertices.clear();
    normals.clear();
    texCoords.clear();
    indices.clear();
    for (int i = 0; i < ELEM_COUNT; i++)
        ranges[i] = {};
    faceMeshes.clear();
}

void insertFaces(RenderMesh *mesh, std::vector<Face> &errFacesOut,
        const std::unordered_map<edge_id, index_t> &edgeIDIndices,
        const std::unordered_map<id_t, std::vector<Face>> &matFaces, const Surface &surf,
        RenderFaceMesh::State state) {
    for (const auto &pair : matFaces) {
        IndexRange range = {mesh->indices.size(), 0};
        for (const auto &face : pair.second) {
            auto normal = mesh->normals[edgeIDIndices.at(face.edge)];
            auto startI = edgeIDIndices.at(face.edge);
            if (!tesselateFace(mesh->indices, surf, face, normal, startI))
                errFacesOut.push_back(face);
        }
        range.count = mesh->indices.size() - range.start;
        auto faceMesh = RenderFaceMesh{pair.first, range, state};
        mesh->faceMeshes.push_back(faceMesh);
    }
}

void generateRenderMesh(RenderMesh *mesh, const EditorState &state) {
    mesh->clear();

    std::unordered_map<edge_id, index_t> edgeIDIndices;
    mesh->vertices.reserve(state.surf.edges.size() + g_drawVerts.size() + 1);
    mesh->normals.reserve(mesh->vertices.capacity());
    edgeIDIndices.reserve(state.surf.edges.size());

    index_t index = 0;
    for (const auto &fp : state.surf.faces) {
        auto normal = faceNormal(state.surf, fp.second);
        glm::mat4x2 texMat = faceTexMat(fp.second.paint, normal);
        auto mat = fp.second.paint->material;
        if (mat == id_t{})
            texMat = glm::mat2x2(0.25f) * texMat; // apply scaling to default texture
        for (auto ep : FaceEdges(state.surf, fp.second)) {
            auto v = ep.second.vert.in(state.surf).pos;
            mesh->vertices.push_back(v);
            mesh->normals.push_back(normal);
            mesh->texCoords.push_back(texMat * glm::vec4(v, 1));
            edgeIDIndices[ep.first] = index++;
        }
    }
    // no normals / texCoords!
    auto drawVertsStartI = index;
    for (const auto &vec : g_drawVerts) {
        mesh->vertices.push_back(vec);
        index++;
    }
    auto hoverI = index;
    mesh->vertices.push_back(g_hover.point);

    if (state.selMode == SEL_ELEMENTS) {
        mesh->ranges[ELEM_REG_VERT].start = mesh->indices.size();
        for (const auto &pair : state.surf.verts) {
            if (!state.selVerts.count(pair.first)) {
                mesh->indices.push_back(edgeIDIndices[pair.second.edge]);
                mesh->ranges[ELEM_REG_VERT].count++;
            }
        }

        if (TOOL_FLAGS[g_tool] & TOOLF_DRAW) {
            if (g_hover.type == PICK_DRAWVERT) {
                mesh->indices.push_back(index_t(drawVertsStartI + g_hover.val));
                mesh->ranges[ELEM_REG_VERT].count++;
            }

            mesh->ranges[ELEM_DRAW_POINT].start = mesh->indices.size();
            for (size_t i = 0; i < g_drawVerts.size(); i++) {
                if (g_hover.type != PICK_DRAWVERT || g_hover.val != i) {
                    mesh->indices.push_back(index_t(drawVertsStartI + i));
                    mesh->ranges[ELEM_DRAW_POINT].count++;
                }
            }
            if (g_hover.type && g_hover.type != PICK_VERT && g_hover.type != PICK_DRAWVERT) {
                mesh->indices.push_back(hoverI);
                mesh->ranges[ELEM_DRAW_POINT].count++;
            }
        }

        mesh->ranges[ELEM_SEL_VERT].start = mesh->indices.size();
        for (const auto &v : state.selVerts) {
            mesh->indices.push_back(edgeIDIndices[v.in(state.surf).edge]);
            mesh->ranges[ELEM_SEL_VERT].count++;
        }

        if (g_hover.type == PICK_DRAWVERT || g_hover.vert.find(state.surf)) {
            mesh->ranges[ELEM_HOV_VERT] = {mesh->indices.size(), 1};
            mesh->indices.push_back(hoverI);
        }

        if (numDrawPoints() + (g_hover.type ? 1 : 0) >= 2) {
            mesh->ranges[ELEM_DRAW_LINE].start = mesh->indices.size();
            if (g_tool == TOOL_KNIFE) {
                mesh->indices.push_back(edgeIDIndices[state.selVerts.begin()->in(state.surf).edge]);
                mesh->ranges[ELEM_DRAW_LINE].count++;
            }
            for (size_t i = 0; i < g_drawVerts.size(); i++) {
                mesh->indices.push_back(index_t(drawVertsStartI + i));
                mesh->ranges[ELEM_DRAW_LINE].count++;
            }
            if (g_hover.type) {
                mesh->indices.push_back(hoverI);
                mesh->ranges[ELEM_DRAW_LINE].count++;
            }
        }

        mesh->ranges[ELEM_SEL_EDGE].start = mesh->indices.size();
        for (const auto &e : state.selEdges) {
            mesh->indices.push_back(edgeIDIndices[e]);
            mesh->indices.push_back(edgeIDIndices[e.in(state.surf).twin]);
            mesh->ranges[ELEM_SEL_EDGE].count += 2;
        }

        if (auto hoverEdge = g_hover.edge.find(state.surf)) {
            mesh->ranges[ELEM_HOV_EDGE] = {mesh->indices.size(), 2};
            mesh->indices.push_back(edgeIDIndices[g_hover.edge]);
            mesh->indices.push_back(edgeIDIndices[hoverEdge->twin]);
        }
    }

    mesh->ranges[ELEM_REG_EDGE].start = mesh->indices.size();
    for (const auto &pair : state.surf.edges) {
        if (isPrimary(pair)) {
            mesh->indices.push_back(edgeIDIndices[pair.first]);
            mesh->indices.push_back(edgeIDIndices[pair.second.twin]);
            mesh->ranges[ELEM_REG_EDGE].count += 2;
        }
    }

    std::vector<Face> errFaces;

    face_id hovFace = {};
    if (g_hover.type && (g_hover.type == PICK_FACE || (TOOL_FLAGS[g_tool] & TOOLF_HOVFACE))) {
        if (auto face = g_hoverFace.find(state.surf)) {
            hovFace = g_hoverFace;
            if (!state.selFaces.count(hovFace)) {
                RenderFaceMesh faceMesh;
                faceMesh.material = face->paint->material;
                faceMesh.range.start = mesh->indices.size();
                faceMesh.state = RenderFaceMesh::HOV;
                auto normal = mesh->normals[edgeIDIndices[face->edge]];
                auto startI = edgeIDIndices[face->edge];
                if (!tesselateFace(mesh->indices, state.surf, *face, normal, startI))
                    errFaces.push_back(*face);
                faceMesh.range.count = mesh->indices.size() - faceMesh.range.start;
                mesh->faceMeshes.push_back(faceMesh);
            }
        }
    }

    static std::unordered_map<id_t, std::vector<Face>> matFaces;
    matFaces.clear();

    for (const auto &pair : state.surf.faces) {
        if (!state.selFaces.count(pair.first) && pair.first != hovFace)
            matFaces[pair.second.paint->material].push_back(pair.second);
    }
    insertFaces(mesh, errFaces, edgeIDIndices, matFaces, state.surf, RenderFaceMesh::REG);
    matFaces.clear();

    for (const auto &f : state.selFaces) {
        const auto &face = f.in(state.surf);
        matFaces[face.paint->material].push_back(face);
    }
    insertFaces(mesh, errFaces, edgeIDIndices, matFaces, state.surf, RenderFaceMesh::SEL);

    mesh->ranges[ELEM_ERR_FACE].start = mesh->indices.size();
    for (const auto &face : errFaces) {
        auto faceStart = mesh->indices.size();
        for (auto ep : FaceEdges(state.surf, face)) {
            auto numIndices = mesh->indices.size();
            if (numIndices - faceStart >= 3) {
                mesh->indices.push_back(mesh->indices[faceStart]);
                mesh->indices.push_back(mesh->indices[numIndices - 1]);
            }
            mesh->indices.push_back(edgeIDIndices.at(ep.first));
        }
    }
    mesh->ranges[ELEM_ERR_FACE].count = mesh->indices.size() - mesh->ranges[ELEM_ERR_FACE].start;
}

} // namespace
