// Operations on surfaces. All functions preserve invariants, and are *not* aware of editor state.

#pragma once
#include "common.h"

#include <vector>
#include <glm/mat4x4.hpp>
#include <immer/set.hpp>
#include "surface.h"

namespace winged {

#ifdef CHROMA_DEBUG
uint32_t name(id_t id); // for debugging
template<typename T, typename V>
uint32_t name(std::pair<T, V> pair) {
    return name(pair.first);
}
#endif

// Create a new vertex/edge in the middle of the given edge
Surface splitEdge(Surface surf, edge_id e, glm::vec3 pos);
// Join two vertices on the same face
Surface joinVerts(Surface surf, edge_id e1, edge_id e2);
// Join two edges on the same face
Surface joinEdges(Surface surf, edge_id e1, edge_id e2);
// Create a new edge between two vertices on the same face
Surface splitFace(Surface surf, edge_id e1, edge_id e2,
    const std::vector<glm::vec3> &points, edge_id *splitEdge, int loopIndex = -1);
// Merge two faces along a chain of edges that joins them (given one edge on the chain)
Surface mergeFaces(Surface surf, edge_id e);
// Creates new quad faces for each side of the given face
Surface extrudeFace(Surface surf, face_id f, const immer::set<edge_id> &extEdges);
// Create a pair of opposing faces from the edge loop
Surface splitEdgeLoop(Surface surf, const std::vector<edge_id> &loop);
// Join two faces into a single edge loop
Surface joinEdgeLoops(Surface surf, edge_id e1, edge_id e2);

Surface makePolygonPlane(Surface surf, const std::vector<glm::vec3> &points, face_id *newFace);

Surface transformVertices(Surface surf, const immer::set<vert_id> &verts, const glm::mat4 &m);
Surface snapVertices(Surface surf, const immer::set<vert_id> &verts, float grid);

Surface assignMaterial(Surface surf, const immer::set<face_id> &faces, id_t material);

Surface duplicate(Surface surf, const immer::set<edge_id> &edges, 
    const immer::set<vert_id> &verts, const immer::set<face_id> &faces);
Surface flipAllNormals(Surface surf);
Surface flipNormals(Surface surf,
    const immer::set<edge_id> &edges, const immer::set<vert_id> &verts);

void validateSurface(const Surface &surf); // debug builds only

} // namespace
