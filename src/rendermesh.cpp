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
    texCoords.clear();
    indices.clear();
    for (int i = 0; i < ELEM_COUNT; i++)
        ranges[i] = {};
    faceMeshes.clear();
}

void insertFaceIndices(RenderMesh *mesh,
        const std::unordered_map<id_t, std::vector<index_t>> &matIndices,
        RenderFaceMesh::State state) {
    for (auto &pair : matIndices) {
        IndexRange range = {mesh->indices.size(), pair.second.size()};
        RenderFaceMesh faceMesh = {pair.first, range, state};
        mesh->faceMeshes.push_back(faceMesh);
        mesh->indices.insert(mesh->indices.end(), pair.second.begin(), pair.second.end());
    }
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
        glm::mat4x2 texMat = faceTexMat(fp.second.paint, normal);
        id_t mat = fp.second.paint->material;
        for (auto &ep : FaceEdges(state.surf, fp.second)) {
            glm::vec3 v = ep.second.vert.in(state.surf).pos;
            mesh->vertices.push_back(v);
            mesh->normals.push_back(normal);
            mesh->texCoords.push_back(texMat * glm::vec4(v, 1));
            edgeIDIndices[ep.first] = index++;
        }
    }
    // no normals / texCoords!
    const index_t drawVertsStartI = index;
    for (auto &vec : g_drawVerts) {
        mesh->vertices.push_back(vec);
        index++;
    }
    const index_t hoverI = index;
    mesh->vertices.push_back(g_hover.point);

    if (state.selMode == SEL_ELEMENTS) {
        mesh->ranges[ELEM_REG_VERT].start = mesh->indices.size();
        for (auto &pair : state.surf.verts) {
            if (!state.selVerts.count(pair.first)) {
                mesh->indices.push_back(edgeIDIndices[pair.second.edge]);
                mesh->ranges[ELEM_REG_VERT].count++;
            }
        }

        if (TOOL_FLAGS[g_tool] & TOOLF_DRAW) {
            if (g_hover.type == PICK_DRAWVERT) {
                mesh->indices.push_back((index_t)(drawVertsStartI + g_hover.val));
                mesh->ranges[ELEM_REG_VERT].count++;
            }

            mesh->ranges[ELEM_DRAW_POINT].start = mesh->indices.size();
            for (size_t i = 0; i < g_drawVerts.size(); i++) {
                if (g_hover.type != PICK_DRAWVERT || g_hover.val != i) {
                    mesh->indices.push_back((index_t)(drawVertsStartI + i));
                    mesh->ranges[ELEM_DRAW_POINT].count++;
                }
            }
            if (g_hover.type && g_hover.type != PICK_VERT && g_hover.type != PICK_DRAWVERT) {
                mesh->indices.push_back(hoverI);
                mesh->ranges[ELEM_DRAW_POINT].count++;
            }
        }

        mesh->ranges[ELEM_SEL_VERT].start = mesh->indices.size();
        for (auto &v : state.selVerts) {
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
                mesh->indices.push_back((index_t)(drawVertsStartI + i));
                mesh->ranges[ELEM_DRAW_LINE].count++;
            }
            if (g_hover.type) {
                mesh->indices.push_back(hoverI);
                mesh->ranges[ELEM_DRAW_LINE].count++;
            }
        }

        mesh->ranges[ELEM_SEL_EDGE].start = mesh->indices.size();
        for (auto e : state.selEdges) {
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
    for (auto &pair : state.surf.edges) {
        if (isPrimary(pair)) {
            mesh->indices.push_back(edgeIDIndices[pair.first]);
            mesh->indices.push_back(edgeIDIndices[pair.second.twin]);
            mesh->ranges[ELEM_REG_EDGE].count += 2;
        }
    }

    std::vector<index_t> errIndices;

    face_id hovFace = {};
    if (g_hover.type && (g_hover.type == PICK_FACE || (TOOL_FLAGS[g_tool] & TOOLF_HOVFACE))) {
        hovFace = g_hoverFace;
        if (!state.selFaces.count(hovFace)) {
            const Face &face = hovFace.in(state.surf);
            RenderFaceMesh faceMesh;
            faceMesh.material = face.paint->material;
            faceMesh.range.start = mesh->indices.size();
            faceMesh.state = RenderFaceMesh::HOV;
            glm::vec3 normal = mesh->normals[edgeIDIndices[face.edge]];
            tesselateFace(mesh->indices, errIndices, state.surf, face, normal, edgeIDIndices);
            faceMesh.range.count = mesh->indices.size() - faceMesh.range.start;
            mesh->faceMeshes.push_back(faceMesh);
        }
    }

    std::unordered_map<id_t, std::vector<index_t>> matIndices;

    for (auto &pair : state.surf.faces) {
        if (!state.selFaces.count(pair.first) && pair.first != hovFace) {
            glm::vec3 normal = mesh->normals[edgeIDIndices[pair.second.edge]];
            tesselateFace(matIndices[pair.second.paint->material], errIndices,
                state.surf, pair.second, normal, edgeIDIndices);
        }
    }
    insertFaceIndices(mesh, matIndices, RenderFaceMesh::REG);
    matIndices.clear();

    for (auto &f : state.selFaces) {
        const Face &face = f.in(state.surf);
        glm::vec3 normal = mesh->normals[edgeIDIndices[face.edge]];
        tesselateFace(matIndices[face.paint->material], errIndices,
            state.surf, face, normal, edgeIDIndices);
    }
    insertFaceIndices(mesh, matIndices, RenderFaceMesh::SEL);

    mesh->ranges[ELEM_ERR_FACE] = {mesh->indices.size(), errIndices.size()};
    mesh->indices.insert(mesh->indices.end(), errIndices.begin(), errIndices.end());
}

} // namespace
