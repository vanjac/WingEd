#include "ops.h"
#include <unordered_map>
#include <glm/common.hpp>
#ifdef CHROMA_DEBUG
#include "winchroma.h"
#endif

namespace winged {

#ifdef CHROMA_DEBUG
uint32_t name(id_t id) {
    if (id == id_t{})
        return 0;
    return (uint32_t)std::hash<GUID>{}(id);
}
#endif


template<typename K, typename V>
static void insertAll(immer::map<K, V> *map, std::initializer_list<std::pair<K, V>> pairs) {
    for (auto &p : pairs)
        *map = std::move(*map).insert(p);
}

template<typename K, typename V>
static void eraseAll(immer::map<K, V> *map, std::initializer_list<K> keys) {
    for (auto &k : keys)
        *map = std::move(*map).erase(k);
}


static std::vector<edge_pair> makeEdgePairs(size_t count) {
    std::vector<edge_pair> edges;
    edges.reserve(count);
    for (int i = 0; i < count; i++)
        edges.push_back(makeEdgePair());
    return edges;
}

static std::vector<vert_pair> makeVertPairs(size_t count) {
    std::vector<vert_pair> verts;
    verts.reserve(count);
    for (int i = 0; i < count; i++)
        verts.push_back(makeVertPair());
    return verts;
}

static std::vector<face_pair> makeFacePairs(size_t count) {
    std::vector<face_pair> faces;
    faces.reserve(count);
    for (int i = 0; i < count; i++)
        faces.push_back(makeFacePair());
    return faces;
}

static void linkTwins(edge_pair *p1, edge_pair *p2) {
    p1->second.twin = p2->first;
    p2->second.twin = p1->first;
}

static void linkNext(edge_pair *prev, edge_pair *next) {
    prev->second.next = next->first;
    next->second.prev = prev->first;
}

static void linkVert(edge_pair *ep, vert_pair *vp) {
    ep->second.vert = vp->first;
    vp->second.edge = ep->first;
}

static void linkFace(edge_pair *ep, face_pair *fp) {
    ep->second.face = fp->first;
    fp->second.edge = ep->first;
}

static Surface assignFaceEdges(Surface surf, Face face, face_id setId) {
    for (auto faceEdge : FaceEdges(surf, face)) {
        faceEdge.second.face = setId;
        insertAll(&surf.edges, {faceEdge});
    }
    return surf;
}

static Surface assignVertEdges(Surface surf, Vertex vert, vert_id setId) {
    for (auto vertEdge : VertEdges(surf, vert)) {
        vertEdge.second.vert = setId;
        insertAll(&surf.edges, {vertEdge});
    }
    return surf;
}


// diagrams created with https://asciiflow.com

Surface splitEdge(Surface surf, edge_id e, glm::vec3 pos) {
    // BEFORE:             ╮
    //                     │
    //                next │ twinPrev
    //                     │
    //         edge        ╰
    // ╭──────────────────╯X
    //         twin          twinVert
    edge_pair edge = e.pair(surf);
    edge_pair twin = edge.second.twin.pair(surf);
    edge_pair next = edge.second.next.pair(surf);
    edge_pair twinPrev = twin.second.prev.pair(surf);
    vert_pair twinVert = twin.second.vert.pair(surf);
    // AFTER:              ╮
    //                     │
    //                next │ twinPrev
    //                     │
    //   edge     newEdge  ╰
    // ╭──────╯X╭─────────╯X
    // twin newVert newTwin  twinVert
    edge_pair newEdge = makeEdgePair();
    edge_pair newTwin = makeEdgePair();
    vert_pair newVert = makeVertPair();

    linkTwins(&newEdge, &newTwin);
    linkNext(&newEdge, &next);
    linkNext(&twinPrev, &newTwin);
    linkNext(&edge, &newEdge);
    linkNext(&newTwin, &twin);

    newVert.second.pos = pos;
    linkVert(&newEdge, &newVert);
    twin.second.vert = newVert.first;
    linkVert(&newTwin, &twinVert);

    newEdge.second.face = edge.second.face;
    newTwin.second.face = twin.second.face;

    insertAll(&surf.edges, {edge, twin, next, twinPrev, newEdge, newTwin});
    insertAll(&surf.verts, {twinVert, newVert});
    return surf;
}

// helper for joinVerts
static Surface joinFaceEdges(Surface surf, edge_pair prev, edge_pair edge, bool *collapsedFace) {
    // AFTER:    X prevVert
    //           ╮
    //           │prev    face
    //   prevTwin│
    //           ╰    edge
    //           X╭──────────╯
    //       vert     twin
    *collapsedFace = edge.second.next == prev.first;
    if (*collapsedFace) {
        edge_pair prevTwin = prev.second.twin.pair(surf);
        edge_pair twin = edge.second.twin.pair(surf);
        vert_pair prevVert = prev.second.vert.pair(surf);
        vert_pair vert = edge.second.vert.pair(surf); // == keepVert
        if (twin.second.prev == prevTwin.first) // triangle merging into line
            throw winged_error(L"These vertices can't be merged");
        linkTwins(&prevTwin, &twin);
        prevVert.second.edge = prevTwin.second.next;
        vert.second.edge = prevTwin.first;

        insertAll(&surf.edges, {prevTwin, twin});
        insertAll(&surf.verts, {prevVert, vert});
        eraseAll(&surf.edges, {prev.first, edge.first});
    } else {
        face_pair face = edge.second.face.pair(surf);
        face.second.edge = edge.first;
        linkNext(&prev, &edge);
        insertAll(&surf.edges, {prev, edge});
        insertAll(&surf.faces, {face});
    }
    return surf;
}

// helper for joinVerts
static Surface joinVertsSharedEdge(Surface surf, edge_pair edge, edge_pair next) {
    // BEFORE:   ╮
    //           │prev
    //       vert╰  edge     next
    //   ╭──────╯X╭──────╯X╭──────╯
    //  twinNext    twin  ╮
    //            twinPrev│
    //                    ╰
    edge_pair prev = edge.second.prev.pair(surf);
    edge_pair twin = edge.second.twin.pair(surf);
    vert_pair vert = edge.second.vert.pair(surf);

    vert.second.edge = twin.second.next;
    insertAll(&surf.verts, {vert});
    eraseAll(&surf.edges, {edge.first, twin.first});

    bool collapsedFace;
    surf = joinFaceEdges(std::move(surf), prev, next, &collapsedFace);
    if (collapsedFace)
        eraseAll(&surf.faces, {edge.second.face});
    edge_pair twinPrev = twin.second.prev.pair(surf);
    edge_pair twinNext = twin.second.next.pair(surf);
    surf = joinFaceEdges(std::move(surf), twinPrev, twinNext, &collapsedFace);
    if (collapsedFace)
        eraseAll(&surf.faces, {twin.second.face});
    return surf;
}

Surface joinVerts(Surface surf, edge_id e1, edge_id e2) {
    // BEFORE:   ╮
    //           │prev1
    //   keepVert╰          edge2 
    //   ╭──────╯X        X╭──────╯
    //     edge1          ╮delVert
    //               prev2│
    //                    ╰
    edge_pair edge1 = e1.pair(surf);
    edge_pair edge2 = e2.pair(surf);
    vert_pair keepVert = edge1.second.vert.pair(surf);
    vert_pair delVert = edge2.second.vert.pair(surf);
    if (edge1.first == edge2.first)
        throw winged_error();
    if (edge1.second.face != edge2.second.face)
        throw winged_error(L"Vertices must share a common face!");
    edge_pair sharedEdge = {}, sharedEdgeNext = {};
    for (auto vertEdge : VertEdges(surf, delVert.second)) {
        vertEdge.second.vert = keepVert.first;
        insertAll(&surf.edges, {vertEdge});
        edge_pair vertEdgeNext = vertEdge.second.next.pair(surf);
        if (vertEdgeNext.second.vert == keepVert.first) {
            sharedEdge = vertEdge;
            sharedEdgeNext = vertEdgeNext;
        }
    }
    edge2.second.vert = keepVert.first;
    eraseAll(&surf.verts, {delVert.first});

    // AFTER:    ╮    face
    //           │prev1
    //           ╰  edge2
    //   ╭──────╯X╭──────╯
    //    edge1  ╮keepVert
    //      prev2│
    // newFace   ╰
    if (sharedEdge.first != edge_id{}) {
        surf = joinVertsSharedEdge(std::move(surf), sharedEdge, sharedEdgeNext);
    } else {
        edge_pair prev1 = edge1.second.prev.pair(surf);
        edge_pair prev2 = edge2.second.prev.pair(surf);
        bool collapsedFace1, collapsedFace2;
        surf = joinFaceEdges(std::move(surf), prev2, edge1, &collapsedFace1);
        prev1 = prev1.first.pair(surf);
        edge2 = edge2.first.pair(surf);
        surf = joinFaceEdges(std::move(surf), prev1, edge2, &collapsedFace2);
        if (collapsedFace1 && collapsedFace2) {
            eraseAll(&surf.faces, {edge1.second.face});
        } else if (!collapsedFace1 && !collapsedFace2) {
            face_pair newFace = makeFacePair();
            newFace.second.edge = edge1.first; // existing face has been assigned to edge2
            surf = assignFaceEdges(std::move(surf), newFace.second, newFace.first);
            insertAll(&surf.faces, {newFace});
        }
    }

    return surf;
}

Surface joinEdges(Surface surf, edge_id e1, edge_id e2) {
    edge_pair edge1 = e1.pair(surf);
    edge_pair edge2 = e2.pair(surf);
    if (edge1.first == edge2.first)
        throw winged_error();
    if (edge1.second.face != edge2.second.face)
        throw winged_error(L"Edges must share a common face!");

    if (edge2.second.next != edge1.first)
        surf = joinVerts(std::move(surf), edge1.first, edge2.second.next);
    if (edge1.second.next != edge2.first)
        surf = joinVerts(std::move(surf), edge1.second.next, edge2.first);
    return surf;
}

Surface splitFace(Surface surf, edge_id e1, edge_id e2,
        const std::vector<glm::vec3> &points, edge_id *splitEdge) {
    // BEFORE:
    // ╮               ╮
    // │prev1     edge2│
    // │               │
    // ╰               ╰
    // X     face      X
    // ╮               ╮
    // │               │
    // │edge1     prev2│
    // ╰               ╰
    edge_pair edge1 = e1.pair(surf);
    edge_pair edge2 = e2.pair(surf);
    edge_pair prev1 = edge1.second.prev.pair(surf);
    face_pair face = edge1.second.face.pair(surf);
    if (edge1.second.face != edge2.second.face) {
        throw winged_error(L"Edges must share a common face!");
    } else if ((edge1.first == edge2.first || edge1.second.next == edge2.first) && points.empty()) {
        // edge already exists between vertices
        *splitEdge = edge1.first;
        return surf;
    } else if (edge2.second.next == edge1.first && points.empty()) {
        *splitEdge = edge2.second.twin;
        return surf;
    } else if (edge1.first == edge2.first && points.size() == 1) {
        throw winged_error(); // would create a two-sided face
    }
    // AFTER:
    // ╮     face      ╮
    // │               │
    // │prev1     edge2│
    // ╰   newEdge1    ╰
    // X╭─────────────╯X
    // ╮   newEdge2    ╮
    // │edge1     prev2│
    // │               │
    // ╰    newFace    ╰
    edge_pair newEdge1 = makeEdgePair();
    edge_pair newEdge2 = makeEdgePair();
    face_pair newFace = makeFacePair();
    *splitEdge = newEdge1.first;

    linkTwins(&newEdge1, &newEdge2);
    linkNext(&prev1, &newEdge1);
    linkNext(&newEdge2, &edge1);
    newEdge1.second.vert = edge1.second.vert;
    linkFace(&newEdge1, &face);

    for (auto &v : points) {
        edge_pair fwdEdge1 = makeEdgePair();
        edge_pair fwdEdge2 = makeEdgePair();
        vert_pair newVert = makeVertPair();

        linkTwins(&fwdEdge1, &fwdEdge2);
        linkNext(&newEdge1, &fwdEdge1);
        linkNext(&fwdEdge2, &newEdge2);
        newVert.second.pos = v;
        linkVert(&fwdEdge1, &newVert);
        newEdge2.second.vert = newVert.first;
        fwdEdge1.second.face = face.first;
        fwdEdge2.second.face = newFace.first;

        insertAll(&surf.edges, {newEdge1, newEdge2, fwdEdge1, fwdEdge2});
        insertAll(&surf.verts, {newVert});
        newEdge1 = fwdEdge1;
        newEdge2 = fwdEdge2;
    }

    insertAll(&surf.edges, {edge1, prev1});
    // refresh these edges, could be the same as other edges
    edge2 = e2.pair(surf);
    edge_pair prev2 = edge2.second.prev.pair(surf);
    linkNext(&newEdge1, &edge2);
    linkNext(&prev2, &newEdge2);
    newEdge2.second.vert = edge2.second.vert;
    newFace.second.edge = newEdge2.first;

    insertAll(&surf.edges, {edge2, prev2, newEdge1, newEdge2});
    insertAll(&surf.faces, {face, newFace});
    surf = assignFaceEdges(std::move(surf), newFace.second, newFace.first);
    return surf;
}

Surface mergeFaces(Surface surf, edge_id e) {
    edge_pair given = e.pair(surf);
    face_pair keepFace = given.second.face.pair(surf);
    face_pair delFace = given.second.twin.in(surf).face.pair(surf);
    if (keepFace.first == delFace.first)
        throw winged_error(L"Deleting this edge would create a hole in the face!");

    surf = assignFaceEdges(std::move(surf), delFace.second, keepFace.first);
    eraseAll(&surf.faces, {delFace.first});

    // find the first edge in chain
    edge_pair edge = given;
    edge_pair twin, prev;
    while (1) {
        twin = edge.second.twin.pair(surf);
        prev = edge.second.prev.pair(surf);
        if (prev.second.twin != twin.second.next)
            break; // more than two edges on vertex
        edge = prev;
        if (edge.first == given.first)
            throw winged_error(L"Can't merge the two sides of a plane!");
    }

    // first bordering edge
    {
        edge_pair twinNext = twin.second.next.pair(surf);
        vert_pair vert = edge.second.vert.pair(surf);
        //      ╮
        //      │prev
        //      ╰      edge
        // vert X╭─────────
        //      ╮      twin
        //      │ twinNext
        //      ╰
        linkNext(&prev, &twinNext);
        vert.second.edge = twinNext.first;
        insertAll(&surf.edges, {prev, twinNext});
        insertAll(&surf.verts, {vert});
    }

    // iterate all edges bordering both faces
    while (1) {
        // prev is no longer valid
        eraseAll(&surf.edges, {edge.first, twin.first});
        {
            edge_pair next = edge.second.next.pair(surf);
            if (next.second.twin != twin.second.prev) {
                // this is the last bordering edge
                edge_pair twinPrev = twin.second.prev.pair(surf);
                vert_pair twinVert = twin.second.vert.pair(surf);
                //           ╮
                //       next│nextTwin
                // edge      ╰
                // ─────────╯X twinVert
                // twin      ╮
                //   twinPrev│
                //           ╰
                twinVert.second.edge = next.first;
                insertAll(&surf.verts, {twinVert});
                bool collapsedFace;
                surf = joinFaceEdges(std::move(surf), twinPrev, next, &collapsedFace);
                if (collapsedFace)
                    eraseAll(&surf.faces, {keepFace.first});
                break;
            }
            edge = next;
            twin = next.second.twin.pair(surf);
        }
        eraseAll(&surf.verts, {edge.second.vert});
    }

    return surf;
}

Surface extrudeFace(Surface surf, face_id f, const immer::set<edge_id> &extEdges) {
    // ┌────────────┐
    // │╲   side   ╱│
    // │ ╲        ╱ │ base (previous edges of face)
    // │  ┌──────┐  │
    // │  │ face │  │
    // │  └──────┘  │
    // │ ╱        ╲ │
    // │╱          ╲│
    // └────────────┘
    face_pair face = f.pair(surf);
    std::vector<edge_pair> topEdges, baseTwins;
    std::vector<vert_pair> baseVerts;
    for (auto topEdge : FaceEdges(surf, face.second)) {
        topEdges.push_back(topEdge);
        baseTwins.push_back(topEdge.second.twin.pair(surf));
        baseVerts.push_back(topEdge.second.vert.pair(surf));
    }
    size_t size = topEdges.size();
    std::vector<edge_pair> baseEdges = makeEdgePairs(size);
    std::vector<edge_pair> topTwins = makeEdgePairs(size);
    std::vector<edge_pair> joinEdges = makeEdgePairs(size);
    std::vector<edge_pair> joinTwins = makeEdgePairs(size);
    std::vector<vert_pair> topVerts = makeVertPairs(size);
    std::vector<face_pair> sideFaces = makeFacePairs(size);
    //    topVert│  topEdge  │    │
    //           X───────────┘    │
    //          ╱   topTwin   ╲   │
    // joinTwin╱               ╲  │
    //        ╱joinEdge         ╲ │
    //       ╱      baseEdge     ╲│
    //      X─────────────────────┘
    // baseVert     baseTwin

    for (size_t i = 0, j = size - 1; i < size; j = i++) {
        bool extrudeEdgeI = extEdges.empty() || extEdges.count(topEdges[i].first);
        bool extrudeEdgeJ = extEdges.empty() || extEdges.count(topEdges[j].first);
        if (!extrudeEdgeI && !extrudeEdgeJ)
            continue;

        linkTwins(&joinEdges[i], &joinTwins[i]);
        if (extrudeEdgeI) {
            linkTwins(&topEdges[i], &topTwins[i]);
            linkTwins(&baseEdges[i], &baseTwins[i]);
            linkNext(&joinEdges[i], &baseEdges[i]);
        } else {
            edge_pair baseTwinNextI = baseTwins[i].second.next.pair(surf);
            if (baseTwinNextI.first == baseTwins[j].first) throw winged_error(L"Can't extrude!");
            linkNext(&joinEdges[i], &baseTwinNextI);
            insertAll(&surf.edges, {baseTwinNextI});
        }
        if (extrudeEdgeJ) {
            linkNext(&baseEdges[j], &joinTwins[i]);
        } else {
            edge_pair baseTwinPrevJ = baseTwins[j].second.prev.pair(surf);
            if (baseTwinPrevJ.first == baseTwins[i].first) throw winged_error(L"Can't extrude!");
            linkNext(&baseTwinPrevJ, &joinTwins[i]);
            insertAll(&surf.edges, {baseTwinPrevJ});
        }
        linkNext(extrudeEdgeI ? &topTwins[i] : &baseTwins[i], &joinEdges[i]);
        linkNext(&joinTwins[i], extrudeEdgeJ ? &topTwins[j] : &baseTwins[j]);

        topVerts[i].second = baseVerts[i].second; // copy position
        linkVert(&joinEdges[i], &topVerts[i]);
        linkVert(&joinTwins[i], &baseVerts[i]);
        topEdges[i].second.vert = topVerts[i].first;
        topTwins[j].second.vert = topVerts[i].first;
        baseEdges[i].second.vert = baseVerts[i].first;
        if (!extrudeEdgeJ) baseTwins[j].second.vert = topVerts[i].first;

        linkFace(&baseEdges[i], &sideFaces[i]);
        topTwins[i].second.face = sideFaces[i].first;
        topEdges[i].second.face = face.first;
        joinEdges[i].second.face = extrudeEdgeI ? sideFaces[i].first : baseTwins[i].second.face;
        joinTwins[i].second.face = extrudeEdgeJ ? sideFaces[j].first : baseTwins[j].second.face;
    }

    face.second.edge = topEdges[0].first;
    insertAll(&surf.faces, {face});
    for (size_t i = 0, j = size - 1; i < size; j = i++) {
        insertAll(&surf.edges, {topEdges[i], baseTwins[i]});
        insertAll(&surf.verts, {baseVerts[i]});
        bool extrudeEdgeI = extEdges.empty() || extEdges.count(topEdges[i].first);
        if (extrudeEdgeI) {
            insertAll(&surf.edges, {baseEdges[i], topTwins[i]});
            insertAll(&surf.faces, {sideFaces[i]});
        }
        if (extrudeEdgeI || extEdges.count(topEdges[j].first)) {
            insertAll(&surf.edges, {joinEdges[i], joinTwins[i]});
            insertAll(&surf.verts, {topVerts[i]});
        }
    }
    return surf;
}

Surface splitEdgeLoop(Surface surf, const std::vector<edge_id> &loop) {
    size_t size = loop.size();
    std::vector<edge_pair> newEdges1 = makeEdgePairs(size);
    std::vector<edge_pair> newEdges2 = makeEdgePairs(size);
    std::vector<vert_pair> newVerts = makeVertPairs(size);
    face_pair newFace1 = makeFacePair();
    face_pair newFace2 = makeFacePair();

    for (size_t i = 0, j = size - 1; i < size; j = i++) {
        edge_pair edge = loop[i].pair(surf);
        edge_pair twin = edge.second.twin.pair(surf);
        edge_pair *newEdge1 = &newEdges1[i];
        edge_pair *newEdge2 = &newEdges2[i];
        edge_pair *lastNewEdge1 = &newEdges1[j];
        edge_pair *lastNewEdge2 = &newEdges2[j];
        vert_pair vert = edge.second.vert.pair(surf);
        vert_pair *newVert = &newVerts[i];

        linkTwins(&edge, newEdge1);
        linkTwins(&twin, newEdge2);
        linkNext(newEdge1, lastNewEdge1);
        linkNext(lastNewEdge2, newEdge2);
        newEdge1->second.face = newFace1.first;
        newEdge2->second.face = newFace2.first;
        newVert->second = vert.second; // copy pos
        linkVert(lastNewEdge1, newVert);
        linkVert(newEdge2, &vert);
        linkVert(&edge, newVert);

        insertAll(&surf.edges, {edge, twin});
        insertAll(&surf.verts, {vert});
    }

    newFace1.second.edge = newEdges1[0].first;
    newFace2.second.edge = newEdges2[0].first;
    insertAll(&surf.faces, {newFace1, newFace2});
    for (size_t i = 0; i < size; i++)
        insertAll(&surf.edges, {newEdges1[i], newEdges2[i]});
    for (size_t i = 0; i < size; i++) {
        vert_pair *newVert = &newVerts[i];
        surf = assignVertEdges(std::move(surf), newVert->second, newVert->first);
        insertAll(&surf.verts, {*newVert});
    }
    return surf;
}

Surface joinEdgeLoops(Surface surf, edge_id e1, edge_id e2) {
    edge_pair edge1 = e1.pair(surf);
    edge_pair edge2 = e2.pair(surf);
    do {
        edge_pair twin2 = edge2.second.twin.pair(surf);
        vert_pair vert1 = edge1.second.vert.pair(surf);
        vert_pair vert2 = twin2.second.vert.pair(surf);
        surf = assignVertEdges(std::move(surf), vert2.second, vert1.first);
        vert1.second.edge = twin2.first;
        insertAll(&surf.verts, {vert1});
        eraseAll(&surf.verts, {vert2.first});
        edge1 = edge1.second.next.pair(surf); edge2 = edge2.second.prev.pair(surf);
    } while (edge1.first != e1 && edge2.first != e2);

    if (edge1.first != e1 || edge2.first != e2)
        throw winged_error(L"Faces have different number of sides!");

    while (1) {
        edge_pair twin1 = edge1.second.twin.pair(surf);
        edge_pair twin2 = edge2.second.twin.pair(surf);
        if (twin1.second.face == edge2.second.face || twin2.second.face == edge1.second.face)
            throw winged_error(L"Faces share an edge!");
        if (twin1.second.face == twin2.second.face)
            throw winged_error(L"Edges share a face!");
        linkTwins(&twin1, &twin2);
        insertAll(&surf.edges, {twin1, twin2});
        eraseAll(&surf.edges, {edge1.first, edge2.first});
        if (edge1.second.next == e1)
            break;
        edge1 = edge1.second.next.pair(surf); edge2 = edge2.second.prev.pair(surf);
    }
    eraseAll(&surf.faces, {edge1.second.face, edge2.second.face});
    return surf;
}

Surface makePolygonPlane(Surface surf, const std::vector<glm::vec3> &points, face_id *newFace) {
    size_t size = points.size();
    if (size < 3)
        throw winged_error();
    std::vector<edge_pair> edges1 = makeEdgePairs(size);
    std::vector<edge_pair> edges2 = makeEdgePairs(size);
    std::vector<vert_pair> verts = makeVertPairs(size);
    face_pair face1 = makeFacePair();
    face_pair face2 = makeFacePair();

    for (size_t i = 0, j = size - 1; i < size; j = i++) {
        edge_pair *edge1 = &edges1[i];
        edge_pair *edge2 = &edges2[i];
        edge_pair *lastEdge1 = &edges1[j];
        edge_pair *lastEdge2 = &edges2[j];
        vert_pair *vert = &verts[i];
        vert_pair *lastVert = &verts[j];

        linkTwins(edge1, edge2);
        linkNext(lastEdge1, edge1);
        linkNext(edge2, lastEdge2);
        edge1->second.face = face1.first;
        edge2->second.face = face2.first;
        lastVert->second.pos = points[i];
        linkVert(edge1, lastVert);
        edge2->second.vert = vert->first;
    }
    face1.second.edge = edges1[0].first;
    face2.second.edge = edges2[0].first;

    for (size_t i = 0; i < size; i++) {
        insertAll(&surf.edges, {edges1[i], edges2[i]});
        insertAll(&surf.verts, {verts[i]});
    }
    insertAll(&surf.faces, {face1, face2});
    *newFace = face1.first;
    return surf;
}

Surface transformVertices(Surface surf, const immer::set<vert_id> &verts, const glm::mat4 &m) {
    for (auto &v : verts) {
        vert_pair vert = v.pair(surf);
        vert.second.pos = m * glm::vec4(vert.second.pos, 1);
        insertAll(&surf.verts, {vert});
    }
    return surf;
}

Surface snapVertices(Surface surf, const immer::set<vert_id> &verts, float grid) {
    for (auto &v : verts) {
        vert_pair vert = v.pair(surf);
        vert.second.pos = glm::roundEven(vert.second.pos / grid) * grid;
        insertAll(&surf.verts, {vert});
    }
    return surf;
}

Surface duplicate(Surface surf, const immer::set<edge_id> &edges, 
        const immer::set<vert_id> &verts, const immer::set<face_id> &faces) {
    std::unordered_map<edge_id, edge_id> edgeMap;
    std::unordered_map<vert_id, vert_id> vertMap;
    std::unordered_map<face_id, face_id> faceMap;
    for (auto e : edges) {
        edgeMap[e] = genId();
        edgeMap[e.in(surf).twin] = genId();
    }
    for (auto v : verts)
        vertMap[v] = genId();
    for (auto f : faces)
        faceMap[f] = genId();

    for (auto pair : edgeMap) {
        edge_pair edge = {pair.second, pair.first.in(surf)};
        edge.second.twin = edgeMap[edge.second.twin];
        edge.second.next = edgeMap[edge.second.next];
        edge.second.prev = edgeMap[edge.second.prev];
        edge.second.vert = vertMap[edge.second.vert];
        edge.second.face = faceMap[edge.second.face];
        insertAll(&surf.edges, {edge});
    }
    for (auto pair : vertMap) {
        vert_pair vert = {pair.second, pair.first.in(surf)};
        vert.second.edge = edgeMap[vert.second.edge];
        insertAll(&surf.verts, {vert});
    }
    for (auto pair : faceMap) {
        face_pair face = {pair.second, pair.first.in(surf)};
        face.second.edge = edgeMap[face.second.edge];
        insertAll(&surf.faces, {face});
    }
    return surf;
}

Surface flipAllNormals(Surface surf) {
    Surface newSurf = surf;
    for (auto edge : surf.edges) {
        std::swap(edge.second.prev, edge.second.next);
        edge.second.vert = edge.second.twin.in(surf).vert;
        insertAll(&newSurf.edges, {edge});
    }
    for (auto vert : surf.verts) {
        vert.second.edge = vert.second.edge.in(surf).twin;
        insertAll(&newSurf.verts, {vert});
    }
    return newSurf;
}

Surface flipNormals(Surface surf,
        const immer::set<edge_id> &edges, const immer::set<vert_id> &verts) {
    for (auto e : edges) {
        edge_pair edge = e.pair(surf);
        edge_pair twin = edge.second.twin.pair(surf);
        std::swap(edge.second.prev, edge.second.next);
        std::swap(twin.second.prev, twin.second.next);
        std::swap(edge.second.vert, twin.second.vert);
        insertAll(&surf.edges, {edge, twin});
    }
    for (auto v : verts) {
        vert_pair vert = v.pair(surf);
        vert.second.edge = vert.second.edge.in(surf).twin;
        insertAll(&surf.verts, {vert});
    }
    return surf;
}


#ifndef CHROMA_DEBUG
void validateSurface(const Surface &) {}
#else
void validateSurface(const Surface &surf) {
    auto tooMany = winged_error(L"Too many geometry errors (see log)");
    #define CHECK_VALID(cond, message, ...)     \
        if (!(cond)) {                          \
            LOG(message, __VA_ARGS__);          \
            if (++invalid > 100) throw tooMany; \
        }

    int invalid = 0;
    for (auto &pair : surf.verts) {
        CHECK_VALID(pair.second.edge.find(surf), "Vert %08X has invalid edge ID %08X!",
            name(pair), name(pair.second.edge));
    }
    for (auto &pair : surf.faces) {
        CHECK_VALID(pair.second.edge.find(surf), "Face %08X has invalid edge ID %08X!",
            name(pair), name(pair.second.edge));
    }
    for (auto &pair : surf.edges) {
        CHECK_VALID(pair.second.twin.find(surf), "Edge %08X has invalid twin ID %08X!",
            name(pair), name(pair.second.twin));
        CHECK_VALID(pair.second.next.find(surf), "Edge %08X has invalid next ID %08X!",
            name(pair), name(pair.second.next));
        CHECK_VALID(pair.second.prev.find(surf), "Edge %08X has invalid prev ID %08X!",
            name(pair), name(pair.second.prev));
        CHECK_VALID(pair.second.vert.find(surf), "Edge %08X has invalid vert ID %08X!",
            name(pair), name(pair.second.vert));
        CHECK_VALID(pair.second.face.find(surf), "Edge %08X has invalid face ID %08X!",
            name(pair), name(pair.second.face));
    }
    if (invalid) {
        LOG("---------");
        throw winged_error(L"Invalid element IDs (see log)"); // can't do any more checks
    }

    for (auto &pair : surf.verts) {
        for (auto &vertEdge : VertEdges(surf, pair.second)) {
            CHECK_VALID(vertEdge.second.vert == pair.first,
                "Edge %08X attached to vert %08X references a different vert %08X!",
                name(vertEdge), name(pair), name(vertEdge.second.vert));
        }
    }
    for (auto &pair : surf.faces) {
        for (auto &faceEdge : FaceEdges(surf, pair.second)) {
            CHECK_VALID(faceEdge.second.face == pair.first,
                "Edge %08X attached to face %08X references a different face %08X!",
                name(faceEdge), name(pair), name(faceEdge.second.face));
        }
    }
    for (auto &pair : surf.edges) {
        CHECK_VALID(pair.second.twin != pair.first, "Edge %08X twin is itself!", name(pair));
        CHECK_VALID(pair.second.next != pair.first, "Edge %08X next is itself!", name(pair));
        CHECK_VALID(pair.second.prev != pair.first, "Edge %08X prev is itself!", name(pair));
        CHECK_VALID(pair.second.twin.in(surf).twin == pair.first,
            "Edge %08X twin %08X has a different twin %08X!",
            name(pair), name(pair.second.twin), name(pair.second.twin.in(surf).twin));
        CHECK_VALID(pair.second.next.in(surf).prev == pair.first,
            "Edge %08X next %08X has a different prev %08X!",
            name(pair), name(pair.second.next), name(pair.second.next.in(surf).prev));
        CHECK_VALID(pair.second.twin.in(surf).vert != pair.second.vert,
            "Edge %08X between single vert %08X!", name(pair), name(pair.second.vert));
        CHECK_VALID(pair.second.next != pair.second.twin,
            "Edge %08X forms an endpoint!", name(pair));
        CHECK_VALID(pair.second.next != pair.second.prev,
            "Edge %08X forms a two-sided face!", name(pair));
        bool foundEdge = false;
        for (auto &faceEdge : FaceEdges(surf, pair.second.face.in(surf))) {
            if (faceEdge.first == pair.first) {
                foundEdge = true;
                break;
            }
        }
        CHECK_VALID(foundEdge, "Edge %08X can't be reached from face %08X!",
            name(pair), name(pair.second.face));
        foundEdge = false;
        for (auto &vertEdge : VertEdges(surf, pair.second.vert.in(surf))) {
            if (vertEdge.first == pair.first) {
                foundEdge = true;
                break;
            }
        }
        CHECK_VALID(foundEdge, "Edge %08X can't be reached from vert %08X!",
            name(pair), name(pair.second.face));
    }
    if (invalid) {
        LOG("---------");
        throw winged_error(L"Invalid geometry (see log)"); // can't do any more checks
    }
    #undef CHECK_VALID
}
#endif

} // namespace
