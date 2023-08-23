#include "file.h"
#include <unordered_map>
#include "winchroma.h"
#include "atlbase.h"

using namespace chroma;

namespace winged {

static void write(HANDLE handle, const void *buf, DWORD size) {
    if (!CHECKERR(WriteFile(handle, buf, size, NULL, NULL)))
        throw winged_error(L"Error writing to file");
}

template<typename K, typename V>
static void writeMap(HANDLE handle, const immer::map<K, V> &map, DWORD vSize = sizeof(V)) {
    write(handle, tempPtr(map.size()), 4);
    for (auto &pair : map) {
        write(handle, &pair.first, sizeof(pair.first));
        write(handle, &pair.second, vSize);
    }
}

template<typename T>
static void writeSet(HANDLE handle, const immer::set<T> &set) {
    write(handle, tempPtr(set.size()), 4);
    for (auto &v : set)
        write(handle, &v, sizeof(v));
}

void writeFile(TCHAR *file, const EditorState &state, const ViewState &view) {
    CHandle handle(CreateFile(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL));
    if (handle == INVALID_HANDLE_VALUE)
        throw winged_error(L"Error saving file");
    write(handle, tempPtr('WING'), 4);
    write(handle, tempPtr(1), 4);
    writeMap(handle, state.surf.verts);
    writeMap(handle, state.surf.faces, sizeof(edge_id));
    writeMap(handle, state.surf.edges);
    writeSet(handle, state.selVerts);
    writeSet(handle, state.selFaces);
    writeSet(handle, state.selEdges);
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

template<typename K, typename V>
static immer::map<K, V> readMap(HANDLE handle, DWORD vSize = sizeof(V)) {
    immer::map<K, V> map;
    uint32_t size = readVal<uint32_t>(handle);
    for (uint32_t i = 0; i < size; i++) {
        K key = readVal<K>(handle);
        V val = readVal<V>(handle, vSize);
        map = std::move(map).set(key, val);
    }
    return map;
}

template<typename T>
static immer::set<T> readSet(HANDLE handle) {
    immer::set<T> set;
    uint32_t size = readVal<uint32_t>(handle);
    for (uint32_t i = 0; i < size; i++)
        set = std::move(set).insert(readVal<T>(handle));
    return set;
}

std::tuple<EditorState, ViewState> readFile(TCHAR *file) {
    CHandle handle(CreateFile(file, GENERIC_READ, 0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL));
    if (handle == INVALID_HANDLE_VALUE)
        throw winged_error(L"Error opening file");
    if (readVal<uint32_t>(handle) != 'WING')
        throw winged_error(L"Unrecognized file format");
    if (readVal<uint32_t>(handle) != 1)
        throw winged_error(L"Unrecognized file version");
    EditorState state;
    state.surf.verts = readMap<vert_id, Vertex>(handle);
    state.surf.faces = readMap<face_id, Face>(handle, sizeof(edge_id));
    state.surf.edges = readMap<edge_id, HEdge>(handle);
    state.selVerts = readSet<vert_id>(handle);
    state.selFaces = readSet<face_id>(handle);
    state.selEdges = readSet<edge_id>(handle);
    read(handle, &state.SAVE_DATA, sizeof(EditorState) - offsetof(EditorState, SAVE_DATA));
    ViewState view = readVal<ViewState>(handle);
    return {state, view};
}


void writeObj(TCHAR *file, const Surface &surf) {
    CHandle handle(CreateFile(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL));
    if (handle == INVALID_HANDLE_VALUE)
        throw winged_error(L"Error saving file");
    char buf[256];

    std::unordered_map<vert_id, int> vertIndices;
    int i = 1;
    for (auto &vert : surf.verts) {
        auto pos = vert.second.pos;
        int len = sprintf(buf, "v %f %f %f\n", pos.x, pos.y, pos.z);
        write(handle, buf, len);
        vertIndices[vert.first] = i++;
    }

    for (auto &face : surf.faces) {
        write(handle, "\nf", 2);
        for (auto edge : FaceEdges(surf, face.second)) {
            int v = vertIndices[edge.second.vert];
            int len = sprintf(buf, " %d", v);
            write(handle, buf, len);
        }
    }
}

} // namespace
