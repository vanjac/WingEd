#include "rendermesh.h"
#include <memory>
#include <unordered_map>
#include "winchroma.h" // required for GLU
#include <gl/GLU.h>
#include <glm/gtc/type_ptr.hpp>
#include "main.h"

namespace winged {

class FaceTesselator {
public:
    FaceTesselator() {
        tess = gluNewTess();
        gluTessCallback(tess, GLU_TESS_BEGIN_DATA, (GLvoid (CALLBACK*) ())beginCallback);
        gluTessCallback(tess, GLU_TESS_END, (GLvoid (CALLBACK*) ())endCallback);
        gluTessCallback(tess, GLU_TESS_VERTEX_DATA, (GLvoid (CALLBACK*) ())vertexCallback);
        gluTessCallback(tess, GLU_TESS_ERROR_DATA, (GLvoid (CALLBACK*) ())errorCallback);
        // gluTessCallback(tess, GLU_TESS_COMBINE_DATA, (GLvoid (*) ())combineCallback);
        // gluTessCallback(tess, GLU_TESS_EDGE_FLAG_DATA, (GLvoid (*) ())edgeFlagCallback);
    }

    void tesselate(std::vector<index_t> &faceIsOut, std::vector<index_t> &errorIsOut,
            const Surface &surf, const Face &face, glm::vec3 normal,
            const std::unordered_map<edge_id, index_t> &edgeIDIndices) {
        // https://www.glprogramming.com/red/chapter11.html
        size_t initialSize = faceIsOut.size();
        indices = &faceIsOut;
        error = 0;
        gluTessNormal(tess, normal.x, normal.y, normal.z);
        gluTessBeginPolygon(tess, this);
        gluTessBeginContour(tess);
        for (auto &ep : FaceEdges(surf, face)) {
            index_t vertI = edgeIDIndices.at(ep.first);
            glm::dvec3 dPos = ep.second.vert.in(surf).pos;
            gluTessVertex(tess, glm::value_ptr(dPos), (void *)vertI);
        }
        gluTessEndContour(tess);
        gluTessEndPolygon(tess);

        if (error) {
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

private:
    GLUtesselator *tess;

    std::vector<index_t> *indices;
    GLenum mode;
    size_t startI;
    GLenum error;

    static void CALLBACK beginCallback(GLenum mode, void *data) {
        auto self = (FaceTesselator *)data;
        self->mode = mode;
        self->startI = self->indices->size();
    }
    static void CALLBACK endCallback() {} // TODO: remove?
    static void CALLBACK vertexCallback(void *vertex, void *data) {
        auto self = (FaceTesselator *)data;
        index_t index = (index_t)(size_t)vertex;
        size_t numIndices = self->indices->size();
        if (self->mode == GL_TRIANGLE_STRIP && numIndices - self->startI >= 3) {
            if ((numIndices - self->startI) % 6 == 0) {
                self->indices->push_back((*self->indices)[numIndices - 3]);
                self->indices->push_back((*self->indices)[numIndices - 1]);
            } else {
                self->indices->push_back((*self->indices)[numIndices - 1]);
                self->indices->push_back((*self->indices)[numIndices - 2]);
            }
        } else if (self->mode == GL_TRIANGLE_FAN && numIndices - self->startI >= 3) {
            self->indices->push_back((*self->indices)[self->startI]);
            self->indices->push_back((*self->indices)[numIndices - 1]);
        }
        self->indices->push_back(index);
    }
    static void CALLBACK errorCallback(GLenum error, void *data) {
        ((FaceTesselator*)data)->error = error;
    }
};

static std::unique_ptr<FaceTesselator> g_tess;

void initRenderMesh() {
    g_tess = std::make_unique<FaceTesselator>();
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
        g_tess->tesselate(*faceIs, mesh->errFaceIs, state.surf, pair.second, normal, edgeIDIndices);
    }
}

} // namespace
