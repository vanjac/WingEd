#include "file.h"
#include <unordered_map>
#include <unordered_set>
#include "winchroma.h"
#include <atlbase.h>

using namespace chroma;

template <class T>
inline std::size_t hashCombine(std::size_t seed, const T& v) {
    // based on boost
    return seed ^ (std::hash<T>{}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

template<>
struct std::hash<std::pair<winged::vert_id, winged::vert_id>> {
    std::size_t operator() (const std::pair<winged::vert_id, winged::vert_id> &key) const {
        return hashCombine(hashCombine(0, key.first), key.second);
    }
};

template<>
struct std::hash<winged::Paint> {
    std::size_t operator() (const winged::Paint &key) const {
        auto hash = hashCombine(0, key.material);
        for (int r = 0; r < 2; r++) {
            for (int c = 0; c < 4; c++)
                hash = hashCombine(hash, key.texAxes[c][r]);
            for (int c = 0; c < 3; c++)
                hash = hashCombine(hash, key.texTF[c][r]);
        }
        return hash;
    }
};

namespace winged {

static bool operator==(const Paint &a, const Paint &b) {
    return a.material == b.material && a.texAxes == b.texAxes && a.texTF == b.texTF;
}

static void write(HANDLE handle, const void *buf, DWORD size) {
    if (!CHECKERR(WriteFile(handle, buf, size, NULL, NULL)))
        throw winged_error(L"Error writing to file");
}

template<typename T, typename U>
static void writeSet(HANDLE handle, const immer::set<T> &set, const std::unordered_map<T, U> &map) {
    write(handle, tempPtr(set.size()), 4);
    for (auto &v : set)
        write(handle, &map.at(v), sizeof(U));
}

void writeFile(const TCHAR *file, const EditorState &state, const ViewState &view) {
    CHandle handle(CreateFile(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL));
    if (handle == INVALID_HANDLE_VALUE)
        throw winged_error(L"Error saving file");
    write(handle, tempPtr('WING'), 4);
    write(handle, tempPtr(2), 4);

    std::unordered_map<Paint, uint32_t> paintIndices;
    std::unordered_map<face_id, uint32_t> faceIndices;
    faceIndices.reserve(state.surf.faces.size());
    std::unordered_map<vert_id, uint32_t> vertIndices;
    vertIndices.reserve(state.surf.verts.size());
    std::unordered_map<edge_id, uint32_t> edgeIndices;
    edgeIndices.reserve(state.surf.edges.size());

    std::vector<uint32_t> facePaintIndices;
    std::vector<Paint> paints;
    for (auto &face : state.surf.faces) {
        auto foundPaint = paintIndices.find(face.second.paint);
        if (foundPaint == paintIndices.end()) {
            paintIndices[face.second.paint] = (uint32_t)paints.size();
            facePaintIndices.push_back((uint32_t)paints.size());
            paints.push_back(face.second.paint);
        } else {
            facePaintIndices.push_back(foundPaint->second);
        }
        faceIndices[face.first] = (uint32_t)faceIndices.size();
    }
    write(handle, tempPtr(paints.size()), 4);
    write(handle, tempPtr(state.surf.faces.size()), 4);
    write(handle, tempPtr(state.surf.verts.size()), 4);
    write(handle, tempPtr(state.surf.edges.size()), 4);

    write(handle, paints.data(), (DWORD)(paints.size() * sizeof(Paint)));
    write(handle, facePaintIndices.data(), (DWORD)(facePaintIndices.size() * sizeof(uint32_t)));

    for (auto &vert : state.surf.verts) {
        write(handle, &vert.second.pos, sizeof(vert.second.pos));
        vertIndices[vert.first] = (uint32_t)vertIndices.size();
    }
    for (auto &face : state.surf.faces) {
        for (auto &edge : FaceEdges(state.surf, face.second)) {
            write(handle, &vertIndices[edge.second.vert], 4);
            edgeIndices[edge.first] = (uint32_t)edgeIndices.size();
        }
        write(handle, tempPtr(-1), 4);
    }

    writeSet(handle, state.selFaces, faceIndices);
    writeSet(handle, state.selVerts, vertIndices);
    writeSet(handle, state.selEdges, edgeIndices);
    write(handle, &state.SAVE_DATA, sizeof(EditorState) - offsetof(EditorState, SAVE_DATA));
    write(handle, &view, sizeof(view));
}

static void read(HANDLE handle, void *buf, DWORD size) {
    if (!CHECKERR(ReadFile(handle, buf, size, NULL, NULL)))
        throw winged_error(L"Error reading file");
}

template<typename T>
static T readVal(HANDLE handle, DWORD size = sizeof(T)) {
    T val;
    read(handle, &val, size);
    return val;
}

template<typename T, typename U>
static immer::set<T> readSet(HANDLE handle, const std::vector<std::pair<T, U>> &vec) {
    immer::set<T> set;
    uint32_t size = readVal<uint32_t>(handle);
    for (uint32_t i = 0; i < size; i++)
        set = std::move(set).insert(vec[readVal<uint32_t>(handle)].first);
    return set;
}

std::tuple<EditorState, ViewState> readFile(const TCHAR *file) {
    CHandle handle(CreateFile(file, GENERIC_READ, 0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL));
    if (handle == INVALID_HANDLE_VALUE)
        throw winged_error(L"Error opening file");
    if (readVal<uint32_t>(handle) != 'WING')
        throw winged_error(L"Unrecognized file format");
    auto version = readVal<uint32_t>(handle);
    if (version != 2)
        throw winged_error(L"Unrecognized file version");
    EditorState state;

    uint32_t numPaints = readVal<uint32_t>(handle);
    uint32_t numFaces = readVal<uint32_t>(handle);
    uint32_t numVerts = readVal<uint32_t>(handle);
    uint32_t numEdges = readVal<uint32_t>(handle);
    std::vector<immer::box<Paint>> paints;
    std::vector<face_pair> faces;
    std::vector<vert_pair> verts;
    std::vector<edge_pair> edges;
    paints.reserve(numPaints);
    faces.reserve(numFaces);
    verts.reserve(numVerts);
    edges.reserve(numEdges);
    for (uint32_t p = 0; p < numPaints; p++) {
        paints.push_back(readVal<Paint>(handle));
    }
    for (uint32_t f = 0; f < numFaces; f++) {
        face_pair pair = {genId(), {}};
        pair.second.paint = paints[readVal<uint32_t>(handle)];
        faces.push_back(pair);
    }
    for (uint32_t v = 0; v < numVerts; v++) {
        vert_pair pair = {genId(), {}};
        pair.second.pos = readVal<glm::vec3>(handle);
        verts.push_back(pair);
    }
    std::unordered_map<std::pair<vert_id, vert_id>, uint32_t> vertPairEdges;
    vertPairEdges.reserve(numEdges);
    for (uint32_t f = 0; f < numFaces; f++) {
        size_t faceEdgeStart = edges.size();
        uint32_t v;
        while ((v = readVal<uint32_t>(handle)) != (uint32_t)-1) {
            edge_pair edge = {genId(), {}};
            edge.second.face = faces[f].first;
            edge.second.vert = verts[v].first;
            verts[v].second.edge = edge.first;
            if (edges.size() == faceEdgeStart) { // first edge
                faces[f].second.edge = edge.first;
            } else {
                edge.second.prev = edges.back().first;
                edges.back().second.next = edge.first;
            }
            edges.push_back(edge);
        };
        edges[faceEdgeStart].second.prev = edges.back().first;
        edges.back().second.next = edges[faceEdgeStart].first;

        for (size_t i = faceEdgeStart; i < edges.size(); i++) {
            edge_pair *edge = &edges[i];
            const edge_pair &next = (i == edges.size() - 1) ?
                edges[faceEdgeStart] : edges[i + 1];
            auto found = vertPairEdges.find({edge->second.vert, next.second.vert});
            if (found != vertPairEdges.end()) {
                uint32_t twinI = found->second;
                edge->second.twin = edges[twinI].first;
                edges[twinI].second.twin = edge->first;
            } else {
                vertPairEdges[{next.second.vert, edge->second.vert}] = (uint32_t)i;
            }
        }
    }
    for (auto &pair : faces)
        state.surf.faces = std::move(state.surf.faces).insert(pair);
    for (auto &pair : verts)
        state.surf.verts = std::move(state.surf.verts).insert(pair);
    for (auto &pair : edges)
        state.surf.edges = std::move(state.surf.edges).insert(pair);
    state.selFaces = readSet<face_id>(handle, faces);
    state.selVerts = readSet<vert_id>(handle, verts);
    state.selEdges = readSet<edge_id>(handle, edges);

    read(handle, &state.SAVE_DATA, sizeof(EditorState) - offsetof(EditorState, SAVE_DATA));
    ViewState view = readVal<ViewState>(handle);
    return {state, view};
}


void writeObj(const TCHAR *file, const Surface &surf) {
    CHandle handle(CreateFile(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL));
    if (handle == INVALID_HANDLE_VALUE)
        throw winged_error(L"Error saving file");
    char buf[256];

    std::unordered_map<vert_id, int> vertIndices;
    int i = 1;
    for (auto &vert : surf.verts) {
        auto pos = vert.second.pos;
        write(handle, buf, sprintf(buf, "v %f %f %f\n", pos.x, pos.y, pos.z));
        vertIndices[vert.first] = i++;
    }

    int vn = 1, vt = 1;
    for (auto &face : surf.faces) {
        glm::vec3 normal = faceNormal(surf, face.second);
        glm::mat4x2 texMat = faceTexMat(face.second.paint, normal);
        write(handle, buf, sprintf(buf, "\nvn %f %f %f", normal.x, normal.y, normal.z));
        for (auto &edge : FaceEdges(surf, face.second)) {
            glm::vec2 uv = texMat * glm::vec4(edge.second.vert.in(surf).pos, 1);
            write(handle, buf, sprintf(buf, "\nvt %f %f", uv.x, uv.y));
        }
        write(handle, "\nf", 2);
        for (auto &edge : FaceEdges(surf, face.second)) {
            int v = vertIndices[edge.second.vert];
            write(handle, buf, sprintf(buf, " %d/%d/%d", v, vt++, vn));
        }
        vn++;
    }
}

} // namespace
