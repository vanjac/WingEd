#include "rendermesh.h"
#include <memory>
#include <unordered_map>
#include "winchroma.h" // required for GLU
#include <gl/GLU.h>
#include <glm/gtc/type_ptr.hpp>
#include "main.h"

namespace winged {

struct FaceTessState {
    std::vector<index_t> *indices;
    GLenum mode;
    size_t startI;
    GLenum error;
};

static GLUtesselator *g_tess;

static void CALLBACK tessBeginCallback(GLenum mode, void *data) {
    auto state = (FaceTessState *)data;
    state->mode = mode;
    state->startI = state->indices->size();
}

static void CALLBACK tessVertexCallback(void *vertex, void *data) {
    auto state = (FaceTessState *)data;
    index_t index = (index_t)(size_t)vertex;
    size_t numIndices = state->indices->size();
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
    ((FaceTessState *)data)->error = error;
}

static void tesselateFace(std::vector<index_t> &faceIsOut, std::vector<index_t> &errorIsOut,
        const Surface &surf, const Face &face, glm::vec3 normal,
        const std::unordered_map<edge_id, index_t> &edgeIDIndices) {
    // https://www.glprogramming.com/red/chapter11.html
    size_t initialSize = faceIsOut.size();
    FaceTessState state;
    state.indices = &faceIsOut;
    state.error = 0;
    gluTessNormal(g_tess, normal.x, normal.y, normal.z);
    gluTessBeginPolygon(g_tess, &state);
    gluTessBeginContour(g_tess);
    for (auto &ep : FaceEdges(surf, face)) {
        index_t vertI = edgeIDIndices.at(ep.first);
        glm::dvec3 dPos = ep.second.vert.in(surf).pos;
        gluTessVertex(g_tess, glm::value_ptr(dPos), (void *)vertI);
    }
    gluTessEndContour(g_tess);
    gluTessEndPolygon(g_tess);

    if (state.error) {
        faceIsOut.erase(faceIsOut.begin() + initialSize, faceIsOut.end());

        size_t errorStart = errorIsOut.size();
        for (auto &ep : FaceEdges(surf, face)) {
            size_t numIndices = errorIsOut.size();
            if (numIndices - errorStart >= 3) {
                errorIsOut.push_back(errorIsOut[errorStart]);
                errorIsOut.push_back(errorIsOut[numIndices - 1]);
            }
            errorIsOut.push_back(edgeIDIndices.at(ep.first));
        }
    }
}

void initRenderMesh() {
    g_tess = gluNewTess();
    gluTessCallback(g_tess, GLU_TESS_BEGIN_DATA, (GLvoid (CALLBACK*) ())tessBeginCallback);
    // gluTessCallback(g_tess, GLU_TESS_END, (GLvoid (CALLBACK*) ())tessEndCallback);
    gluTessCallback(g_tess, GLU_TESS_VERTEX_DATA, (GLvoid (CALLBACK*) ())tessVertexCallback);
    gluTessCallback(g_tess, GLU_TESS_ERROR_DATA, (GLvoid (CALLBACK*) ())tessErrorCallback);
    // gluTessCallback(g_tess, GLU_TESS_COMBINE_DATA, (GLvoid (*) ())tessCombineCallback);
    // gluTessCallback(g_tess, GLU_TESS_EDGE_FLAG_DATA, (GLvoid (*) ())tessEdgeFlagCallback);
}

void RenderMesh::clear() {
    vertices.clear();
    normals.clear();
    regVertIs.clear();
    selVertIs.clear();
    hovVertIs.clear();
    regEdgeIs.clear();
    selEdgeIs.clear();
    hovEdgeIs.clear();
    regFaceIs.clear();
    selFaceIs.clear();
    hovFaceIs.clear();
    errFaceIs.clear();
    drawPointIs.clear();
    drawLineIs.clear();
}

void generateRenderMesh(RenderMesh *mesh, const EditorState &state) {
    mesh->clear();

    std::unordered_map<edge_id, index_t> edgeIDIndices;
    mesh->vertices.reserve(state.surf.edges.size() + g_drawVerts.size() + 1);
    mesh->normals.reserve(mesh->vertices.capacity());
    edgeIDIndices.reserve(state.surf.edges.size());

    index_t index = 0;
    for (auto &fp : state.surf.faces) {
        glm::vec3 normal = faceNormal(state.surf, fp.second);
        for (auto &ep : FaceEdges(state.surf, fp.second)) {
            mesh->vertices.push_back(ep.second.vert.in(state.surf).pos);
            mesh->normals.push_back(normal);
            edgeIDIndices[ep.first] = index++;
        }
    }
    // no normals!
    const index_t drawVertsStartI = index;
    for (auto &vec : g_drawVerts) {
        mesh->vertices.push_back(vec);
        index++;
    }
    const index_t hoverI = index;
    mesh->vertices.push_back(g_hover.point);

    if (state.selMode == SEL_ELEMENTS) {
        for (auto &pair : state.surf.verts) {
            if (state.selVerts.count(pair.first)) {
                mesh->selVertIs.push_back(edgeIDIndices[pair.second.edge]);
            } else {
                mesh->regVertIs.push_back(edgeIDIndices[pair.second.edge]);
            }
        }
        if (TOOL_FLAGS[g_tool] & TOOLF_DRAW) {
            if (numDrawPoints() > 0) {
                for (size_t i = 0; i < g_drawVerts.size(); i++) {
                    if (g_hover.type == PICK_DRAWVERT && g_hover.val == i) {
                        mesh->regVertIs.push_back((index_t)(drawVertsStartI + i));
                    } else {
                        mesh->drawPointIs.push_back((index_t)(drawVertsStartI + i));
                    }
                }
            }
            if (g_hover.type && g_hover.type != PICK_VERT && g_hover.type != PICK_DRAWVERT) {
                mesh->drawPointIs.push_back(hoverI);
            }
        }

        if (g_hover.type == PICK_DRAWVERT || g_hover.vert.find(state.surf)) {
            mesh->hovVertIs.push_back(hoverI);
        }

        if (numDrawPoints() + (g_hover.type ? 1 : 0) >= 2) {
            if (g_tool == TOOL_KNIFE) {
                mesh->drawLineIs.push_back(
                    edgeIDIndices[state.selVerts.begin()->in(state.surf).edge]);
            }
            for (size_t i = 0; i < g_drawVerts.size(); i++)
                mesh->drawLineIs.push_back((index_t)(drawVertsStartI + i));
            if (g_hover.type)
                mesh->drawLineIs.push_back(hoverI);
        }

        for (auto e : state.selEdges) {
            mesh->selEdgeIs.push_back(edgeIDIndices[e]);
            mesh->selEdgeIs.push_back(edgeIDIndices[e.in(state.surf).twin]);
        }
        if (auto hoverEdge = g_hover.edge.find(state.surf)) {
            mesh->hovEdgeIs.push_back(edgeIDIndices[g_hover.edge]);
            mesh->hovEdgeIs.push_back(edgeIDIndices[hoverEdge->twin]);
        }
    }

    for (auto &pair : state.surf.edges) {
        if (isPrimary(pair)) {
            mesh->regEdgeIs.push_back(edgeIDIndices[pair.first]);
            mesh->regEdgeIs.push_back(edgeIDIndices[pair.second.twin]);
        }
    }

    for (auto &pair : state.surf.faces) {
        glm::vec3 normal = mesh->normals[edgeIDIndices[pair.second.edge]];
        std::vector<index_t> *faceIs;
        if (state.selFaces.count(pair.first)) {
            faceIs = &mesh->selFaceIs;
        } else if (g_hover.type && pair.first == g_hoverFace
                && (g_hover.type == PICK_FACE || (TOOL_FLAGS[g_tool] & TOOLF_HOVFACE))) {
            faceIs = &mesh->hovFaceIs;
        } else {
            faceIs = &mesh->regFaceIs;
        }
        tesselateFace(*faceIs, mesh->errFaceIs, state.surf, pair.second, normal, edgeIDIndices);
    }
}

} // namespace
