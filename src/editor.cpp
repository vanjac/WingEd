#include "editor.h"
#include <immer/set_transient.hpp>
#include <glm/common.hpp>
#include "macros.h"

namespace winged {

bool hasSelection(EditorState state) {
    return !state.selVerts.empty() || !state.selFaces.empty() || !state.selEdges.empty();
}

immer::set<vert_id> selAttachedVerts(const EditorState &state) {
    auto verts = state.selVerts.transient();
    for (let &e : state.selEdges) {
        let &edge = e.in(state.surf), &twin = edge.twin.in(state.surf);
        verts.insert(edge.vert);
        verts.insert(twin.vert);
    }
    for (let &f : state.selFaces) {
        for (let faceEdge : FaceEdges(state.surf, f.in(state.surf))) {
            verts.insert(faceEdge.second.vert);
        }
    }
    return verts.persistent();
}

EditorState clearSelection(EditorState state) {
    state.selVerts = {};
    state.selEdges = {};
    state.selFaces = {};
    return state;
}

EditorState cleanSelection(const EditorState &state) {
    EditorState newState = state;
    for (let &vert : state.selVerts)
        if (!vert.find(state.surf))
            newState.selVerts = std::move(newState.selVerts).erase(vert);
    for (let &face : state.selFaces)
        if (!face.find(state.surf))
            newState.selFaces = std::move(newState.selFaces).erase(face);
    for (let &e : state.selEdges) {
        if (let edge = e.find(state.surf)) {
            if (!isPrimary({e, *edge}))
                newState.selEdges = std::move(newState.selEdges).erase(e).insert(edge->twin);
        } else {
            newState.selEdges = std::move(newState.selEdges).erase(e);
        }
    }
    return newState;
}

glm::vec3 vertsCenter(const Surface &surf, immer::set<vert_id> verts) {
    if (verts.empty())
        return {};
    glm::vec3 min = verts.begin()->in(surf).pos, max = min;
    for (let &v : verts) {
        let pos = v.in(surf).pos;
        min = glm::min(min, pos);
        max = glm::max(max, pos);
    }
    return (min + max) / 2.0f;
}

} // namespace
