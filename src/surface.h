// Persistent half-edge mesh data structure
// https://en.wikipedia.org/wiki/Doubly_connected_edge_list
// https://cs184.eecs.berkeley.edu/sp19/article/15/the-half-edge-data-structure

#pragma once
#include "common.h"

#include <utility>
#include <glm/vec3.hpp>
#include <immer/map.hpp>
#include "id.h"
#include "mathutil.h"

namespace winged {

struct Vertex; struct Face; struct HEdge;
struct vert_id; struct face_id; struct edge_id;
struct Surface;

// useful for intermediate construction
using vert_pair = std::pair<vert_id, Vertex>;
using face_pair = std::pair<face_id, Face>;
using edge_pair = std::pair<edge_id, HEdge>;
const vert_pair makeVertPair();
const face_pair makeFacePair();
const edge_pair makeEdgePair();

struct vert_id : id_t {
    vert_id() = default;
    vert_id(const id_t &id) : id_t(id) {}
    const Vertex & in(const Surface &surf) const; // shortcut for surf.verts[id]
    const Vertex * find(const Surface &surf) const;
    const vert_pair pair(const Surface &surf) const;
};
struct face_id : id_t {
    face_id() = default;
    face_id(const id_t &id) : id_t(id) {}
    const Face & in(const Surface &surf) const; // shortcut for surf.faces[id]
    const Face * find(const Surface &surf) const;
    const face_pair pair(const Surface &surf) const;
};
struct edge_id : id_t {
    edge_id() = default;
    edge_id(const id_t &id) : id_t(id) {}
    const HEdge & in(const Surface &surf) const; // shortcut for surf.edges[id]
    const HEdge * find(const Surface &surf) const;
    const edge_pair pair(const Surface &surf) const;
};

} // namespace


template<>
struct std::hash<winged::vert_id> {
    std::size_t operator() (const winged::vert_id &key) const { return std::hash<GUID>{}(key); }
};
template<>
struct std::hash<winged::face_id> {
    std::size_t operator() (const winged::face_id &key) const { return std::hash<GUID>{}(key); }
};
template<>
struct std::hash<winged::edge_id> {
    std::size_t operator() (const winged::edge_id &key) const { return std::hash<GUID>{}(key); }
};


namespace winged {

// Vertices connect at least two edges
struct Vertex {
    edge_id edge = {}; // any outgoing
    // Invariant: edge->vert == this, edge->twin->next->twin->next->twin->next...->vert == this
    // (note: id_t is not actually a pointer, this is simplified notation)

    glm::vec3 pos = {};
};

// Faces must be simple polygons, may be concave but may not contain holes
struct Face {
    edge_id edge = {}; // any bordering
    // Invariant: edge->face == this, edge->next->next->next...vert == this

    id_t material = {};
};

// "Half-Edge"
// Each edge connects two vertices and two faces. A Half-Edge is one side of an edge.
struct HEdge {
    edge_id twin = {}, next = {}, prev = {};
    vert_id vert = {}; // "from" vertex
    face_id face = {};
    // Invariants:
    // next != prev  (no two-sided faces)
    // prev != twin && next != twin  (no vertices with only one edge)
    // twin != this && next != this && prev != this
    // twin->twin == this
    // next->prev == this && prev->next == this
    // twin->vert != vert
    // Edge must be reachable by following face->next->next->next...
    // Edge must be reachable by following vert->twin->next->twin->next->twin->next...

    // Allowed special cases:
    // - Double-sided plane (sides may have different edges)
    // - twin->face == face:
    //     ┌───┬───┐
    //     │   │   │  (this is a single connected face enclosing another face)
    //     │  ┌┴┐  │  (one of its edges borders itself)
    //     │  └─┘  │
    //     └───────┘
};


struct Surface {
    immer::map<vert_id, Vertex> verts;
    immer::map<face_id, Face>   faces;
    immer::map<edge_id, HEdge>  edges;
};

// for each pair of twins there is one primary edge (arbitrary)
bool isPrimary(const edge_pair &pair);
edge_id primaryEdge(const edge_pair &pair);

glm::vec3 faceNormalNonUnit(const Surface &surf, const Face &face); // faster than faceNormal
glm::vec3 faceNormal(const Surface &surf, const Face &face);
Plane facePlane(const Surface &surf, const Face &face);

/* iteration utils */

struct EdgeIter {
    const Surface &surf;
    edge_id edge;
    bool first;
    EdgeIter(const Surface &surf, edge_id edge, bool first);

    const edge_pair operator*() const;
    friend bool operator==(const EdgeIter &a, const EdgeIter &b);
    friend bool operator!=(const EdgeIter &a, const EdgeIter &b);
};

// edges surrounding face (counter-clockwise)
struct FaceEdges {
    struct Iter : EdgeIter {
        Iter(const Surface &surf, edge_id edge, bool first);
        Iter & operator++();
    };

    const Surface &surf; // TODO is this safe?
    Face face;
    FaceEdges(const Surface &surf, Face face);

    Iter begin() const;
    Iter end() const;
};

// outgoing edges from vertex (clockwise)
struct VertEdges {
    struct Iter : EdgeIter {
        Iter(const Surface &surf, edge_id edge, bool first);
        Iter & operator++();
    };

    const Surface &surf;
    Vertex vert;
    VertEdges(const Surface &surf, Vertex vert);

    Iter begin() const;
    Iter end() const;
};

} // namespace
