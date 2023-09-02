#include "surface.h"
#include <glm/geometric.hpp>
#include "mathutil.h"

namespace winged {

const Vertex & vert_id::in(const Surface &surf) const { return surf.verts.at(*this); }
const Face   & face_id::in(const Surface &surf) const { return surf.faces.at(*this); }
const HEdge  & edge_id::in(const Surface &surf) const { return surf.edges.at(*this); }
const Vertex * vert_id::find(const Surface &surf) const { return surf.verts.find(*this); }
const Face   * face_id::find(const Surface &surf) const { return surf.faces.find(*this); }
const HEdge  * edge_id::find(const Surface &surf) const { return surf.edges.find(*this); }
const vert_pair vert_id::pair(const Surface &surf) const { return {*this, in(surf)}; }
const face_pair face_id::pair(const Surface &surf) const { return {*this, in(surf)}; }
const edge_pair edge_id::pair(const Surface &surf) const { return {*this, in(surf)}; }

const vert_pair makeVertPair() { return {genId(), Vertex{}}; }
const face_pair makeFacePair() { return {genId(), Face{}}; }
const edge_pair makeEdgePair() { return {genId(), HEdge{}}; }


const id_t Paint::HOLE_MATERIAL =
    {0x233844da, 0x2edd, 0x4a59, {0xad, 0x49, 0x50, 0x9f, 0x15, 0x60, 0xe9, 0xaa}};
const immer::box<Paint> Face::DEF_PAINT;


bool isPrimary(const edge_pair &pair) {
    return memcmp(&pair.first, &pair.second.twin, sizeof(edge_id)) < 0;
}

edge_id primaryEdge(const edge_pair &pair) {
    return isPrimary(pair) ? pair.first : pair.second.twin;
}

glm::vec3 faceNormalNonUnit(const Surface &surf, const Face &face) {
    glm::vec3 normal = {};
    for (auto &pair : FaceEdges(surf, face)) {
        glm::vec3 v1 = pair.second.vert.in(surf).pos;
        glm::vec3 v2 = pair.second.next.in(surf).vert.in(surf).pos;
        normal += accumPolyNormal(v1, v2);
    }
    return normal;
}

glm::vec3 faceNormal(const Surface &surf, const Face &face) {
    return glm::normalize(faceNormalNonUnit(surf, face));
}

Plane facePlane(const Surface &surf, const Face &face) {
    return {face.edge.in(surf).vert.in(surf).pos, faceNormal(surf, face)};
}

glm::mat4x2 faceTexMat(const Paint &paint, glm::vec3 normal) {
    glm::mat4x2 texAxes = paint.texAxes;
    if (texAxes == glm::mat4x2{}) {
        int axis = maxAxis(glm::abs(normal));
        texAxes[(axis == 0) ? 2 : 0] = glm::vec2(((normal[axis] < 0) ^ (axis == 2)) ? 1 : -1, 0);
        texAxes[(axis == 1) ? 2 : 1] = glm::vec2(0, 1);
    }
    glm::mat4x3 expand = texAxes;
    expand[2][2] = 0;
    expand[3][2] = 1;
    return paint.texTF * expand;
}


EdgeIter::EdgeIter(const Surface &surf, edge_id edge, bool first)
    : surf(surf), edge(edge), first(first) {}

const edge_pair EdgeIter::operator*() const {
    return edge.pair(surf);
}

bool operator==(const EdgeIter &a, const EdgeIter &b) {
    return a.edge == b.edge && !a.first && !b.first;
}
bool operator!=(const EdgeIter &a, const EdgeIter &b) {
    return !(a == b);
}

FaceEdges::FaceEdges(const Surface &surf, Face face) : surf(surf), face(face) {}

FaceEdges::Iter FaceEdges::begin() const {
    return Iter(surf, face.edge, true);
}

FaceEdges::Iter FaceEdges::end() const {
    return Iter(surf, face.edge, false);
}

FaceEdges::Iter::Iter(const Surface &surf, edge_id edge, bool first)
    : EdgeIter(surf, edge, first) {}

FaceEdges::Iter & FaceEdges::Iter::operator++() {
    edge = edge.in(surf).next;
    first = false;
    return *this;
}

VertEdges::VertEdges(const Surface &surf, Vertex vert) : surf(surf), vert(vert) {}

VertEdges::Iter VertEdges::begin() const {
    return Iter(surf, vert.edge, true);
}

VertEdges::Iter VertEdges::end() const {
    return Iter(surf, vert.edge, false);
}

VertEdges::Iter::Iter(const Surface &surf, edge_id edge, bool first)
    : EdgeIter(surf, edge, first) {}

VertEdges::Iter & VertEdges::Iter::operator++() {
    edge = edge.in(surf).twin.in(surf).next;
    first = false;
    return *this;
}

} // namespace
