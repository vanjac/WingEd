#pragma once
#include "common.h"

#include <exception>
#include "surface.h"

namespace winged {

struct winged_error : std::exception {
    const wchar_t *message = NULL;
    winged_error() = default;
    winged_error(const wchar_t *message) : message(message) {}
};

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
Surface splitFace(Surface surf, edge_id e1, edge_id e2);
// Merge two faces along a chain of edges that joins them (given one edge on the chain)
Surface mergeFaces(Surface surf, edge_id e);
// Creates new quad faces for each side of the given face
Surface extrudeFace(Surface surf, face_id f);
Surface joinEdgeLoops(Surface surf, edge_id e1, edge_id e2);

Surface moveVertex(Surface surf, vert_id v, glm::vec3 amount);
Surface scaleVertex(Surface surf, vert_id v, glm::vec3 center, glm::vec3 factor);

Surface flipNormals(Surface surf);

void validateSurface(const Surface &surf); // slow!

Surface makeCube();

} // namespace
