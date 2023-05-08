#include "ops.h"
#include "winchroma.h"

namespace winged {

uint32_t name(id_t id) {
    return (uint32_t)std::hash<GUID>{}(id);
}


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

// helper for mergeVerts
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
        if (twin.second.prev == prevTwin.first) // triangle plane
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

// helper for mergeVerts
static Surface mergeVertsSharedEdge(Surface surf, edge_pair edge, edge_pair prev, edge_pair next) {
    // BEFORE:   ╮
    //           │prev
    //       vert╰  edge     next
    //   ╭──────╯X╭──────╯X╭──────╯
    //  twinNext    twin  ╮
    //            twinPrev│
    //                    ╰
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

Surface mergeVerts(Surface surf, edge_id e1, edge_id e2) {
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
    surf = assignVertEdges(std::move(surf), delVert.second, keepVert.first);
    edge2.second.vert = keepVert.first;
    eraseAll(&surf.verts, {delVert.first});

    edge_pair prev1 = edge1.second.prev.pair(surf);
    edge_pair prev2 = edge2.second.prev.pair(surf);

    // AFTER:    ╮    face
    //           │prev1
    //           ╰  edge2
    //   ╭──────╯X╭──────╯
    //    edge1  ╮keepVert
    //      prev2│
    // newFace   ╰
    if (prev2.first == edge1.first) {
        surf = mergeVertsSharedEdge(std::move(surf), edge1, prev1, edge2);
    } else if (prev1.first == edge2.first) {
        surf = mergeVertsSharedEdge(std::move(surf), edge2, prev2, edge1);
    } else {
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

Surface splitFace(Surface surf, edge_id e1, edge_id e2) {
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
    edge_pair prev2 = edge2.second.prev.pair(surf);
    face_pair face = edge1.second.face.pair(surf);
    if (edge1.second.face != edge2.second.face) {
        throw winged_error(L"Edges must share a common face!");
    } else if (edge1.first == edge2.first
            || edge1.second.next == edge2.first || edge2.second.next == edge1.first) {
        // edge already exists between vertices
        return surf; // just do nothing
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

    linkTwins(&newEdge1, &newEdge2);
    linkNext(&newEdge1, &edge2);
    linkNext(&newEdge2, &edge1);
    linkNext(&prev1, &newEdge1);
    linkNext(&prev2, &newEdge2);

    newEdge1.second.vert = edge1.second.vert;
    newEdge2.second.vert = edge2.second.vert;

    linkFace(&newEdge1, &face);
    newFace.second.edge = newEdge2.first;
    insertAll(&surf.edges, {edge1, edge2, prev1, prev2, newEdge1, newEdge2});
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
                linkNext(&twinPrev, &next);
                twinVert.second.edge = next.first;
                keepFace.second.edge = next.first;
                insertAll(&surf.edges, {twinPrev, next});
                insertAll(&surf.verts, {twinVert});
                insertAll(&surf.faces, {keepFace});
                break;
            }
            edge = next;
            twin = next.second.twin.pair(surf);
        }
        eraseAll(&surf.verts, {edge.second.vert});
    }

    return surf;
}

Surface extrudeFace(Surface surf, face_id f) {
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
    edge_pair topPrev, topFirst;
    edge_id topFirstId = {};
    for (auto baseEdge : FaceEdges(surf, face.second)) {
        //    topVert│  topEdge  │    │
        //           X───────────┘    │
        //          ╱   topTwin   ╲   │
        // joinTwin╱               ╲  │
        //        ╱ joinEdge        ╲ │
        //       ╱      baseEdge     ╲│
        //      ──────────────────────┘
        edge_pair topEdge = makeEdgePair();
        edge_pair topTwin = makeEdgePair();
        edge_pair joinEdge = makeEdgePair();
        edge_pair joinTwin = makeEdgePair();
        vert_pair topVert = makeVertPair();
        face_pair sideFace = makeFacePair();

        linkTwins(&topEdge, &topTwin);
        linkTwins(&joinEdge, &joinTwin);
        // incomplete side face loop
        linkNext(&topTwin, &joinEdge);
        linkNext(&joinEdge, &baseEdge);

        topVert.second = baseEdge.second.vert.in(surf); // copy position
        linkVert(&joinEdge, &topVert);
        topEdge.second.vert = topVert.first;
        joinTwin.second.vert = baseEdge.second.vert;

        linkFace(&joinEdge, &sideFace);
        topTwin.second.face = sideFace.first;
        baseEdge.second.face = sideFace.first;
        topEdge.second.face = face.first;

        // top face loop
        if (topPrev.first != edge_id{}) {
            linkNext(&topPrev, &topEdge);
            if (topFirst.first == edge_id{})
                topFirst = topPrev;
            else
                insertAll(&surf.edges, {topPrev});
        }
        topPrev = topEdge;

        insertAll(&surf.edges, {baseEdge, topTwin, joinEdge, joinTwin});
        insertAll(&surf.verts, {topVert});
        insertAll(&surf.faces, {sideFace});
    }
    linkNext(&topPrev, &topFirst);
    face.second.edge = topFirst.first;
    insertAll(&surf.edges, {topFirst, topPrev});
    insertAll(&surf.faces, {face});

    for (auto &topEdge : FaceEdges(surf, face.second)) {
        //           topNext│    │
        //         topEdge  │    │
        //      X───────────┘    │
        //     ╱   topTwin   ╲   │
        //    ╱               ╲  │
        //   ╱      topTwinPrev╲ │
        //  ╱    baseEdge       ╲│
        // ──────────────────────┘
        edge_pair topTwin = topEdge.second.twin.pair(surf);
        const edge_pair topNext = topEdge.second.next.pair(surf);
        edge_pair topTwinPrev = topNext.second.twin.in(surf).next.in(surf).twin.pair(surf);
        edge_pair baseEdge = topTwin.second.next.in(surf).next.pair(surf);
        // complete side face loop
        linkNext(&topTwinPrev, &topTwin);
        linkNext(&baseEdge, &topTwinPrev);
        topTwinPrev.second.face = topTwin.second.face;
        topTwin.second.vert = topNext.second.vert;
        insertAll(&surf.edges, {topTwin, topTwinPrev, baseEdge});
    }
    return surf;
}

Surface moveVertex(Surface surf, vert_id v, glm::vec3 amount) {
    vert_pair vert = v.pair(surf);
    vert.second.pos += amount;
    insertAll(&surf.verts, {vert});
    return surf;
}

Surface scaleVertex(Surface surf, vert_id v, glm::vec3 center, glm::vec3 factor) {
    vert_pair vert = v.pair(surf);
    vert.second.pos = (vert.second.pos - center) * factor + center;
    insertAll(&surf.verts, {vert});
    return surf;
}

Surface flipNormals(Surface surf) {
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


void validateSurface(const Surface &surf) {
    #define CHECK_VALID(cond, message, ...) \
        if (!(cond)) {                  \
            LOG(message, __VA_ARGS__);  \
            valid = false;              \
        }

    bool valid = true;
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
    if (!valid) {
        LOG("---------");
        throw winged_error(L"Invalid element IDs (see log)"); // can't do any more checks
    }

    for (auto &pair : surf.verts) {
        for (auto &vertEdge : VertEdges(surf, pair.second)) {
            CHECK_VALID(vertEdge.second.vert == pair.first,
                "Edge %08X attached to vert %08X references a different vert %08X!",
                name(vertEdge), name(pair), name(vertEdge.second.vert));
        }
        CHECK_VALID(pair.second.edge.in(surf).twin.in(surf).next != pair.second.edge,
            "Vert %08X only has one edge %08X!", name(pair), name(pair.second.edge));
    }
    for (auto &pair : surf.faces) {
        for (auto &faceEdge : FaceEdges(surf, pair.second)) {
            CHECK_VALID(faceEdge.second.face == pair.first,
                "Edge %08X attached to face %08X references a different face %08X!",
                name(faceEdge), name(pair), name(faceEdge.second.face));
        }
        CHECK_VALID(pair.second.edge.in(surf).next.in(surf).next != pair.second.edge,
            "Face %08X only has two sides!", name(pair));
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
    if (!valid) {
        LOG("---------");
        throw winged_error(L"Invalid geometry (see log)"); // can't do any more checks
    }
    #undef CHECK_VALID
}


Surface makeCube() {
    vert_pair cubeVerts[8];
    for (int i = 0; i < 8; i++) cubeVerts[i] = makeVertPair();
    face_pair cubeFaces[6];
    for (int i = 0; i < 6; i++) cubeFaces[i] = makeFacePair();
    edge_pair cubeEdges[6][4];
    for (int f = 0; f < 6; f++)
        for (int e = 0; e < 4; e++)
            cubeEdges[f][e] = makeEdgePair();

    for (int i = 0; i < 8; i++) {
        Vertex *vert = &cubeVerts[i].second;
        vert->pos.x = (i & 0x1) ? 1.0f : -1.0f;
        vert->pos.y = (i & 0x2) ? 1.0f : -1.0f;
        vert->pos.z = (i & 0x4) ? 1.0f : -1.0f;
    }

    for (int f = 0; f < 6; f++) {
        cubeFaces[f].second.edge = cubeEdges[f][0].first;
        for (int e2 = 0, e1 = 3; e2 < 4; e1 = e2++) {
            cubeEdges[f][e2].second.face = cubeFaces[f].first;
            linkNext(&cubeEdges[f][e1], &cubeEdges[f][e2]);
        }
    }

    // normal (-1, 0, 0)                  0bZYX
    linkVert(&cubeEdges[0][0], &cubeVerts[0b000]);  linkTwins(&cubeEdges[0][0], &cubeEdges[2][3]);
    linkVert(&cubeEdges[0][1], &cubeVerts[0b100]);  linkTwins(&cubeEdges[0][1], &cubeEdges[5][3]);
    linkVert(&cubeEdges[0][2], &cubeVerts[0b110]);
    linkVert(&cubeEdges[0][3], &cubeVerts[0b010]);
    // normal (1, 0, 0)
    linkVert(&cubeEdges[1][0], &cubeVerts[0b001]);  linkTwins(&cubeEdges[1][0], &cubeEdges[4][2]);
    linkVert(&cubeEdges[1][1], &cubeVerts[0b011]);  linkTwins(&cubeEdges[1][1], &cubeEdges[3][2]);
    linkVert(&cubeEdges[1][2], &cubeVerts[0b111]);
    linkVert(&cubeEdges[1][3], &cubeVerts[0b101]);
    // normal (0, -1, 0)
    linkVert(&cubeEdges[2][0], &cubeVerts[0b000]);  linkTwins(&cubeEdges[2][0], &cubeEdges[4][3]);
    linkVert(&cubeEdges[2][1], &cubeVerts[0b001]);  linkTwins(&cubeEdges[2][1], &cubeEdges[1][3]);
    linkVert(&cubeEdges[2][2], &cubeVerts[0b101]);
    linkVert(&cubeEdges[2][3], &cubeVerts[0b100]);
    // normal (0, 1, 0)
    linkVert(&cubeEdges[3][0], &cubeVerts[0b010]);  linkTwins(&cubeEdges[3][0], &cubeEdges[0][2]);
    linkVert(&cubeEdges[3][1], &cubeVerts[0b110]);  linkTwins(&cubeEdges[3][1], &cubeEdges[5][2]);
    linkVert(&cubeEdges[3][2], &cubeVerts[0b111]);
    linkVert(&cubeEdges[3][3], &cubeVerts[0b011]);
    // normal (0, 0, -1)
    linkVert(&cubeEdges[4][0], &cubeVerts[0b000]);  linkTwins(&cubeEdges[4][0], &cubeEdges[0][3]);
    linkVert(&cubeEdges[4][1], &cubeVerts[0b010]);  linkTwins(&cubeEdges[4][1], &cubeEdges[3][3]);
    linkVert(&cubeEdges[4][2], &cubeVerts[0b011]);
    linkVert(&cubeEdges[4][3], &cubeVerts[0b001]);
    // normal (0, 0, 1)
    linkVert(&cubeEdges[5][0], &cubeVerts[0b100]);  linkTwins(&cubeEdges[5][0], &cubeEdges[2][2]);
    linkVert(&cubeEdges[5][1], &cubeVerts[0b101]);  linkTwins(&cubeEdges[5][1], &cubeEdges[1][2]);
    linkVert(&cubeEdges[5][2], &cubeVerts[0b111]);
    linkVert(&cubeEdges[5][3], &cubeVerts[0b110]);

    Surface surf;
    for (int i = 0; i < 8; i++)
        insertAll(&surf.verts, {cubeVerts[i]});
    for (int i = 0; i < 6; i++)
        insertAll(&surf.faces, {cubeFaces[i]});
    for (int f = 0; f < 6; f++)
        for (int e = 0; e < 4; e++)
            insertAll(&surf.edges, {cubeEdges[f][e]});
    return surf;
}

} // namespace
