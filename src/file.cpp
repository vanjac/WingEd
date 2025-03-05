#include "file.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include "winchroma.h"
#include <shlwapi.h>
#include "rendermesh.h"
#include "stdutil.h"

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
struct std::hash<glm::vec2> {
    std::size_t operator() (const glm::vec2 &key) const {
        return hashCombine(hashCombine(0, key.x), key.y);
    }
};

template<>
struct std::hash<glm::vec3> {
    std::size_t operator() (const glm::vec3 &key) const {
        return hashCombine(hashCombine(hashCombine(0, key.x), key.y), key.z);
    }
};

template<>
struct std::hash<winged::Paint> {
    std::size_t operator() (const winged::Paint &key) const {
        auto hash = hashCombine(0, key.material);
        for (int c = 0; c < 4; c++)
            hash = hashCombine(hash, key.texAxes[c]);
        for (int c = 0; c < 3; c++)
            hash = hashCombine(hash, key.texTF[c]);
        return hash;
    }
};

namespace winged {

// Based on ATL
class CHandle {
private:
    HANDLE _handle;
public:
    CHandle(HANDLE handle) : _handle(handle) {}
    ~CHandle() {
        CloseHandle(_handle);
    }
    CHandle(CHandle& other) = delete;
    CHandle& operator=(CHandle& other) = delete;

    operator HANDLE() { return _handle; }
};

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
    for (const auto &v : set)
        write(handle, &map.at(v), sizeof(U));
}

static void writeString(HANDLE handle, const wchar_t *str) {
    auto bufSize = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL);
    if (bufSize > 0) {
        std::vector<char> utf8(bufSize);
        auto len = uint16_t(WideCharToMultiByte(CP_UTF8, 0, str, -1,
            utf8.data(), int(utf8.size()), NULL, NULL));
        write(handle, &len, 2);
        write(handle, utf8.data(), len);
    }
}

void writeFile(const wchar_t *file, const EditorState &state, const ViewState &view,
        const Library &library) {
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
    std::unordered_set<id_t> usedFiles;
    for (const auto &face : state.surf.faces) {
        if (auto paintIndex = tryGet(paintIndices, *face.second.paint)) {
            facePaintIndices.push_back(*paintIndex);
        } else {
            paintIndices[face.second.paint] = uint32_t(paints.size());
            facePaintIndices.push_back(uint32_t(paints.size()));
            paints.push_back(face.second.paint);
            usedFiles.insert(face.second.paint->material);
        }
        faceIndices[face.first] = uint32_t(faceIndices.size());
    }
    write(handle, tempPtr(paints.size()), 4);
    write(handle, tempPtr(state.surf.faces.size()), 4);
    write(handle, tempPtr(state.surf.verts.size()), 4);
    write(handle, tempPtr(state.surf.edges.size()), 4);

    write(handle, paints.data(), DWORD(paints.size() * sizeof(Paint)));
    write(handle, facePaintIndices.data(), DWORD(facePaintIndices.size() * sizeof(uint32_t)));

    for (const auto &vert : state.surf.verts) {
        write(handle, &vert.second.pos, sizeof(vert.second.pos));
        vertIndices[vert.first] = uint32_t(vertIndices.size());
    }
    for (const auto &face : state.surf.faces) {
        for (auto edge : FaceEdges(state.surf, face.second)) {
            write(handle, &vertIndices[edge.second.vert], 4);
            edgeIndices[edge.first] = uint32_t(edgeIndices.size());
        }
        write(handle, tempPtr(-1), 4);
    }

    writeSet(handle, state.selFaces, faceIndices);
    writeSet(handle, state.selVerts, vertIndices);
    writeSet(handle, state.selEdges, edgeIndices);
    write(handle, &state.SAVE_DATA, sizeof(EditorState) - offsetof(EditorState, SAVE_DATA));
    write(handle, &view, sizeof(view));

    for (const auto &id : usedFiles) {
        if (auto path = tryGet(library.idPaths, id)) {
            wchar_t relative[MAX_PATH] = L"";
            if (library.rootPath.empty()) {
                PathRelativePathTo(relative, file, 0, path->c_str(), 0);
            } else {
                PathRelativePathTo(relative, library.rootPath.c_str(), FILE_ATTRIBUTE_DIRECTORY,
                    path->c_str(), 0);
            }
            if (relative[0]) {
                writeString(handle, relative);
            } else {
                writeString(handle, path->c_str()); // absolute path
            }
            write(handle, &id, sizeof(id));
        }
    }
    writeString(handle, L"");
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
    auto size = readVal<uint32_t>(handle);
    for (uint32_t i = 0; i < size; i++)
        set = std::move(set).insert(vec[readVal<uint32_t>(handle)].first);
    return set;
}

static void readString(HANDLE handle, wchar_t *str, size_t size) {
    auto len = readVal<uint16_t>(handle);
    std::unique_ptr<char[]> buf(new char[len + 1]);
    read(handle, buf.get(), len);
    buf[len] = 0;
    MultiByteToWideChar(CP_UTF8, 0, buf.get(), -1, str, int(size));
}

std::tuple<EditorState, ViewState, Library> readFile(const wchar_t *file,
        const wchar_t *libraryPath) {
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

    auto numPaints = readVal<uint32_t>(handle);
    auto numFaces = readVal<uint32_t>(handle);
    auto numVerts = readVal<uint32_t>(handle);
    auto numEdges = readVal<uint32_t>(handle);
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
        auto faceEdgeStart = edges.size();
        uint32_t v;
        while ((v = readVal<uint32_t>(handle)) != uint32_t(-1)) {
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
            auto edge = &edges[i];
            const auto &next = (i == edges.size() - 1) ? edges[faceEdgeStart] : edges[i + 1];
            if (auto twinI = tryGet(vertPairEdges, {edge->second.vert, next.second.vert})) {
                edge->second.twin = edges[*twinI].first;
                edges[*twinI].second.twin = edge->first;
            } else {
                vertPairEdges[{next.second.vert, edge->second.vert}] = uint32_t(i);
            }
        }
    }
    for (const auto &pair : faces)
        state.surf.faces = std::move(state.surf.faces).insert(pair);
    for (const auto &pair : verts)
        state.surf.verts = std::move(state.surf.verts).insert(pair);
    for (const auto &pair : edges)
        state.surf.edges = std::move(state.surf.edges).insert(pair);
    state.selFaces = readSet<face_id>(handle, faces);
    state.selVerts = readSet<vert_id>(handle, verts);
    state.selEdges = readSet<edge_id>(handle, edges);

    read(handle, &state.SAVE_DATA, sizeof(EditorState) - offsetof(EditorState, SAVE_DATA));
    auto view = readVal<ViewState>(handle);

    Library library;
    library.rootPath = libraryPath;
    wchar_t folder[MAX_PATH];
    if (libraryPath[0] == 0) {
        lstrcpy(folder, file);
        PathRemoveFileSpec(folder);
    }
    while (1) {
        wchar_t relative[MAX_PATH] = L"", combined[MAX_PATH] = L"";
        readString(handle, relative, _countof(relative));
        if (relative[0] == 0)
            break;
        if (libraryPath[0] == 0)
            PathCombine(combined, folder, relative);
        else
            PathCombine(combined, libraryPath, relative);
        auto id = readVal<id_t>(handle);
        if (combined[0])
            library.addFile(id, combined);
    }

    return {state, view, library};
}


struct ObjFaceVert {
    int v, vt;
};

void writeObj(const wchar_t *file, const Surface &surf, const Library &library,
        const wchar_t *mtlName, bool writeMtl) {
    std::unordered_map<std::wstring, id_t> matNames;

    {
        CHandle handle(CreateFile(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL));
        if (handle == INVALID_HANDLE_VALUE)
            throw winged_error(L"Error saving OBJ file");
        char buf[256];

        if (mtlName && mtlName[0])
            write(handle, buf, sprintf(buf, "mtllib %S\n\n", mtlName));

        std::unordered_map<vert_id, int> vertIndices;
        int v = 1;
        for (const auto &vert : surf.verts) {
            auto pos = vert.second.pos;
            write(handle, buf, sprintf(buf, "v %f %f %f\n", pos.x, pos.y, pos.z));
            vertIndices[vert.first] = v++;
        }

        std::unordered_map<id_t, std::vector<Face>> matFaces;
        for (const auto &pair : surf.faces) {
            if (pair.second.paint->material != Paint::HOLE_MATERIAL)
                matFaces[pair.second.paint->material].push_back(pair.second);
        }

        std::unordered_map<glm::vec3, int> normalIndices;
        std::unordered_map<glm::vec2, int> texCoordIndices;
        std::vector<ObjFaceVert> faceVerts;
        std::vector<index_t> faceIndices;
        for (const auto &pair : matFaces) {
            std::wstring texFile;
            if (auto path = tryGet(library.idPaths, pair.first)) {
                texFile = PathFindFileName(path->c_str());
                std::replace(texFile.begin(), texFile.end(), L' ', L'_');
            } else {
                texFile = L"default";
            }
            std::wstring matName = texFile;
            int num = 1;
            while (matName.empty() || matNames.count(matName))
                matName = texFile + std::to_wstring(num++);
            matNames[matName] = pair.first;
            write(handle, buf, sprintf(buf, "\nusemtl %S", matName.c_str()));

            for (const auto &face : pair.second) {
                auto normal = faceNormal(surf, face);
                int vn;
                if (auto vnPtr = tryGet(normalIndices, normal)) {
                    vn = *vnPtr;
                } else {
                    vn = int(normalIndices.size()) + 1;
                    normalIndices[normal] = vn;
                    write(handle, buf, sprintf(buf, "\nvn %f %f %f", normal.x, normal.y, normal.z));
                }

                glm::mat4x2 texMat = faceTexMat(face.paint, normal);
                faceVerts.clear();
                for (auto edge : FaceEdges(surf, face)) {
                    auto texCoord = texMat * glm::vec4(edge.second.vert.in(surf).pos, 1);
                    int vt;
                    if (auto vtPtr = tryGet(texCoordIndices, texCoord)) {
                        vt = *vtPtr;
                    } else {
                        vt = int(texCoordIndices.size()) + 1;
                        texCoordIndices[texCoord] = vt;
                        write(handle, buf, sprintf(buf, "\nvt %f %f", texCoord.x, texCoord.y));
                    }
                    faceVerts.push_back({vertIndices[edge.second.vert], vt});
                }

                faceIndices.clear();
                tesselateFace(faceIndices, surf, face, normal);
                for (size_t i = 0; i < faceIndices.size(); ) {
                    write(handle, "\nf", 2);
                    for (size_t j = 0; j < 3; j++, i++) {
                        const auto &ofv = faceVerts[faceIndices[i]];
                        write(handle, buf, sprintf(buf, " %d/%d/%d", ofv.v, ofv.vt, vn));
                    }
                }
                vn++;
            }
        }
    }

    if (writeMtl) {
        wchar_t folder[MAX_PATH], mtlPath[MAX_PATH];
        lstrcpy(folder, file);
        PathRemoveFileSpec(folder);
        PathCombine(mtlPath, folder, mtlName);

        CHandle handle(CreateFile(mtlPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL));
        if (handle == INVALID_HANDLE_VALUE)
            throw winged_error(L"Error saving MTL file");
        char buf[256];

        for (const auto &pair : matNames) {
            write(handle, buf, sprintf(buf, "newmtl %S\n", pair.first.c_str()));
            if (auto texPath = tryGet(library.idPaths, pair.second)) {
                wchar_t relative[MAX_PATH] = L"";
                PathRelativePathTo(relative, folder, FILE_ATTRIBUTE_DIRECTORY, texPath->c_str(), 0);
                for (wchar_t *c = relative; *c; c++)
                    if (*c == L'\\') *c = L'/';
                write(handle, buf, sprintf(buf, "map_Kd %S\n", relative));
            }
        }
    }
}

} // namespace
