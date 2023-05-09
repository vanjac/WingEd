#include "file.h"
#include "winchroma.h"
#include "atlbase.h"

using namespace chroma;

namespace winged {

static void write(HANDLE handle, const void *buf, DWORD size) {
    if (!CHECKERR(WriteFile(handle, buf, size, NULL, NULL)))
        throw winged_error(L"Error writing to file");
}

template<typename K, typename V>
static void writeMap(HANDLE handle, const immer::map<K, V> &map) {
    write(handle, tempPtr(map.size()), 4);
    for (auto &pair : map) {
        write(handle, &pair.first, sizeof(pair.first));
        write(handle, &pair.second, sizeof(pair.second));
    }
}

void writeFile(TCHAR *file, const Surface &surf) {
    CHandle handle(CreateFile(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL));
    if (handle == INVALID_HANDLE_VALUE)
        throw winged_error(L"Error saving file");
    write(handle, tempPtr('WING'), 4);
    write(handle, tempPtr(1), 4);
    writeMap(handle, surf.verts);
    writeMap(handle, surf.faces);
    writeMap(handle, surf.edges);
}

static void read(HANDLE handle, void *buf, DWORD size) {
    if (!CHECKERR(ReadFile(handle, buf, size, NULL, NULL)))
        throw winged_error(L"Error reading file");
}

template<typename T>
static T readVal(HANDLE handle) {
    T val;
    read(handle, &val, sizeof(val));
    return val;
}

template<typename K, typename V>
static immer::map<K, V> readMap(HANDLE handle) {
    immer::map<K, V> map;
    uint32_t size = readVal<uint32_t>(handle);
    for (uint32_t i = 0; i < size; i++) {
        K key = readVal<K>(handle);
        V val = readVal<V>(handle);
        map = std::move(map).set(key, val);
    }
    return map;
}

Surface readFile(TCHAR *file) {
    CHandle handle(CreateFile(file, GENERIC_READ, 0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL));
    if (handle == INVALID_HANDLE_VALUE)
        throw winged_error(L"Error opening file");
    if (readVal<uint32_t>(handle) != 'WING')
        throw winged_error(L"Unrecognized file format");
    if (readVal<uint32_t>(handle) != 1)
        throw winged_error(L"Unrecognized file version");
    Surface surf;
    surf.verts = readMap<vert_id, Vertex>(handle);
    surf.faces = readMap<face_id, Face>(handle);
    surf.edges = readMap<edge_id, HEdge>(handle);
    return surf;
}

} // namespace
