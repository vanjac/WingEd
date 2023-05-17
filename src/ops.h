#pragma once
#include "common.h"

#include <vector>
#include <immer/set.hpp>
#include "surface.h"

namespace winged {

uint32_t name(id_t id); // for debugging
template<typename T, typename V>
uint32_t name(std::pair<T, V> pair) {
    return name(pair.first);
}

// Create a new vertex/edge in the middle of the given edge
Surface splitEdge(Surface surf, edge_id e, glm::vec3 pos);
// Join two vertices on the same face
Surface mergeVerts(Surface surf, edge_id e1, edge_id e2);
// Create a new edge between two vertices on the same face
Surface splitFace(Surface surf, edge_id e1, edge_id e2,
    const std::vector<glm::vec3> points, edge_id *splitEdge);
// Merge two faces along a chain of edges that joins them (given one edge on the chain)
Surface mergeFaces(Surface surf, edge_id e);
// Creates new quad faces for each side of the given face
Surface extrudeFace(Surface surf, face_id f);
// Create a pair of opposing faces from the edge loop
Surface splitEdgeLoop(Surface surf, const std::vector<edge_id> &loop);
// Join two faces into a single edge loop
Surface joinEdgeLoops(Surface surf, edge_id e1, edge_id e2);

Surface makePolygonPlane(Surface surf, const std::vector<glm::vec3> points, face_id *newFace);

Surface moveVertex(Surface surf, vert_id v, glm::vec3 amount);
Surface scaleVertex(Surface surf, vert_id v, glm::vec3 center, glm::vec3 factor);

Surface flipNormals(Surface surf,
    const immer::set<edge_id> &edges, const immer::set<vert_id> &verts);

void validateSurface(const Surface &surf); // slow!

Surface makeCube();

} // namespace
