#include "common.h"
#include "winchroma.h"

#include <stack>
#include <queue>
#include <unordered_set>
#include <gl/GL.h>
#include <gl/GLU.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "editor.h"
#include "ops.h"
#include "picking.h"
#include "file.h"
#include "mathutil.h"
#include "resource.h"
#include <immer/set_transient.hpp>

#pragma comment(lib, "Rpcrt4.lib")
#pragma comment(lib, "Opengl32.lib")
#pragma comment(lib, "Glu32.lib")

using namespace chroma;
using namespace winged;

const TCHAR APP_NAME[] = _T("WingEd");

enum Tool {
    TOOL_SELECT, TOOL_SCALE, TOOL_KNIFE, TOOL_JOIN, NUM_TOOLS
};
struct ToolInfo {
    HCURSOR cursor;
    uint32_t allowedSelModes;
};
const ToolInfo tools[] = {
    {LoadCursor(NULL, IDC_ARROW), (uint32_t)-1},
    {LoadCursor(NULL, IDC_SIZEWE), (uint32_t)-1},
    {LoadCursor(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_KNIFE)), 1<<SEL_ELEMENTS},
    {LoadCursor(NULL, IDC_CROSS), 1<<SEL_ELEMENTS},
};

enum MouseMode {
    MOUSE_NONE = 0, MOUSE_ADJUST, MOUSE_CAM_ROTATE, MOUSE_CAM_PAN
};


static EditorState g_state;
static std::stack<EditorState> g_undoStack;
static std::stack<EditorState> g_redoStack;
static TCHAR g_fileName[MAX_PATH] = {0};

static Tool g_tool = TOOL_SELECT;
static PickResult g_hover;
static face_id g_hoverFace;
static MouseMode g_mouseMode = MOUSE_NONE;
static POINT g_curLockPos;
static glm::vec2 g_moveAccum;
static bool g_flashSel = false;
static std::vector<glm::vec3> g_knifeVerts;

static ViewState g_view;
static glm::mat4 g_projMat, g_mvMat;
static glm::vec2 g_windowDim;

static GLUtesselator *g_tess;
static GLenum g_tess_error;


static EditorState cleanSelection(const EditorState &state) {
    EditorState newState = state;
    for (auto &vert : state.selVerts)
        if (!vert.find(state.surf))
            newState.selVerts = std::move(newState.selVerts).erase(vert);
    for (auto &face : state.selFaces)
        if (!face.find(state.surf))
            newState.selFaces = std::move(newState.selFaces).erase(face);
    for (auto &e : state.selEdges) {
        if (auto edge = e.find(state.surf)) {
            if (!isPrimary({e, *edge}))
                newState.selEdges = std::move(newState.selEdges).erase(e).insert(edge->twin);
        } else {
            newState.selEdges = std::move(newState.selEdges).erase(e);
        }
    }
    return newState;
}

static void pushUndo() {
    g_undoStack.push(g_state);
    g_redoStack = {};
}

static void pushUndo(EditorState newState) {
    validateSurface(newState.surf);
    pushUndo();
    g_state = cleanSelection(newState);
}

static bool hasSelection(EditorState state) {
    return !state.selVerts.empty() || !state.selFaces.empty() || !state.selEdges.empty();
}

static immer::set<vert_id> selAttachedVerts(const EditorState &state) {
    auto verts = state.selVerts.transient();
    for (auto e : state.selEdges) {
        auto edge = e.in(state.surf), twin = edge.twin.in(state.surf);
        verts.insert(edge.vert);
        verts.insert(twin.vert);
    }
    for (auto f : state.selFaces) {
        for (auto &faceEdge : FaceEdges(state.surf, f.in(state.surf))) {
            verts.insert(faceEdge.second.vert);
        }
    }
    return verts.persistent();
}

static EditorState clearSelection(EditorState state) {
    state.selVerts = {};
    state.selEdges = {};
    state.selFaces = {};
    return state;
}

static EditorState select(EditorState state, Surface::ElementType type, id_t id) {
    if (state.selMode == SEL_ELEMENTS) {
        switch (type) {
            case Surface::VERT:
                if (state.surf.verts.count(id))
                    state.selVerts = std::move(state.selVerts).insert(id);
                break;
            case Surface::FACE:
                if (state.surf.faces.count(id))
                    state.selFaces = std::move(state.selFaces).insert(id);
                break;
            case Surface::EDGE:
                if (state.surf.edges.count(id))
                    state.selEdges = std::move(state.selEdges).insert(id);
                break;
        }
    } else if (state.selMode == SEL_SOLIDS) {
        if (auto face = ((face_id)id).find(state.surf)) {
            auto verts = state.selVerts.transient();
            auto faces = state.selFaces.transient();
            auto edges = state.selEdges.transient();
            auto visited = std::unordered_set<edge_id>();
            // flood-fill
            std::queue<edge_id> toSelect;
            toSelect.push(face->edge);
            while (!toSelect.empty()) {
                edge_id e = toSelect.front();
                toSelect.pop();
                if (!visited.count(e)) {
                    edge_pair edge = e.pair(state.surf);
                    visited.insert(e);
                    if (isPrimary(edge))
                        edges.insert(e);
                    verts.insert(edge.second.vert);
                    faces.insert(edge.second.face);
                    toSelect.push(edge.second.twin);
                    toSelect.push(edge.second.next);
                }
            }
            state.selVerts = verts.persistent();
            state.selFaces = faces.persistent();
            state.selEdges = edges.persistent();
        }
    }
    return state;
}

static Surface::ElementType hoverType() {
    switch (g_hover.type) {
        case Surface::VERT: return g_hover.vert.find(g_state.surf) ? Surface::VERT : Surface::NONE;
        case Surface::FACE: return g_hover.face.find(g_state.surf) ? Surface::FACE : Surface::NONE;
        case Surface::EDGE: return g_hover.edge.find(g_state.surf) ? Surface::EDGE : Surface::NONE;
        default: return Surface::NONE;
    }
}

static const HEdge expectSingleSelEdge() {
    if (g_state.selEdges.size() == 1)
        return g_state.selEdges.begin()->in(g_state.surf);
    throw winged_error(L"No selected edge");
}

static edge_pair edgeOnHoverFace(const Surface &surf, vert_id v) {
    // TODO: what if there are multiple?
    for (auto &edge : VertEdges(surf, v.in(surf))) {
        if (edge.second.face == g_hoverFace)
            return edge;
    }
    throw winged_error();
}

static std::pair<edge_id, edge_id> findClosestOpposingEdges(
        const Surface &surf, Face face1, Face face2) {
    float closestDist = FLT_MAX;
    edge_id e1 = face1.edge, e2 = face2.edge;
    for (auto &f1Edge : FaceEdges(surf, face1)) {
        glm::vec3 v1 = f1Edge.second.vert.in(surf).pos;
        for (auto &f2Edge : FaceEdges(surf, face2)) {
            float dist = glm::distance(v1, f2Edge.second.vert.in(surf).pos);
            if (dist < closestDist) {
                e1 = f1Edge.first;
                e2 = f2Edge.second.prev;
                closestDist = dist;
            }
        }
    }
    return {e1, e2};
}

static std::vector<edge_id> sortEdgeLoop(const Surface &surf, immer::set<edge_id> edges) {
    std::vector<edge_id> loop;
    auto edgesTrans = edges.transient();
    loop.reserve(edges.size());
    loop.push_back(*edges.begin());
    edgesTrans.erase(loop[0]);
    while (loop.size() != edges.size()) {
        vert_id nextVert = loop.back().in(surf).next.in(surf).vert;
        edge_id foundEdge = {};
        for (auto &e : edgesTrans) {
            HEdge edge = e.in(surf);
            if (edge.vert == nextVert && edge.twin != loop.back()) {
                foundEdge = e;
                break;
            } else if (edge.twin.in(surf).vert == nextVert && e != loop.back()) {
                foundEdge = edge.twin;
                break;
            }
        }
        if (foundEdge == edge_id{})
            throw winged_error(L"Edges must form a loop");
        loop.push_back(foundEdge);
        edgesTrans.erase(foundEdge);
    }
    if (loop.back().in(surf).next.in(surf).vert != loop[0].in(surf).vert)
        throw winged_error(L"Edges must form a loop");
    return loop;
}


static void refresh(HWND wnd) {
    InvalidateRect(wnd, NULL, false);
}

static void refreshImmediate(HWND wnd) {
    RedrawWindow(wnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void updateStatus(HWND wnd) {
    TCHAR buf[64];
    TCHAR *str = buf;

    if (g_tool == TOOL_KNIFE && g_state.selVerts.size() == 1 && g_hover.type) {
        glm::vec3 lastPos = g_knifeVerts.empty()
            ? g_state.selVerts.begin()->in(g_state.surf).pos
            : g_knifeVerts.back();
        str += _stprintf(str, L"Len: %f    ", glm::distance(lastPos, g_hover.point));
    } else if (g_state.selEdges.size() == 1) {
        edge_pair edge = g_state.selEdges.begin()->pair(g_state.surf);
        glm::vec3 v1 = edge.second.vert.in(g_state.surf).pos;
        glm::vec3 v2 = edge.second.twin.in(g_state.surf).vert.in(g_state.surf).pos;
        str += _stprintf(str, L"Len: %f    ", glm::distance(v1, v2));
    }

    size_t numSel = g_state.selVerts.size() + g_state.selFaces.size() + g_state.selEdges.size();
    if (numSel) {
        uint32_t selName = 0;
        if (!g_state.selVerts.empty()) selName = name(*g_state.selVerts.begin());
        else if (!g_state.selFaces.empty()) selName = name(*g_state.selFaces.begin());
        else if (!g_state.selEdges.empty()) selName = name(*g_state.selEdges.begin());
        str += _stprintf(str, L"Sel: %08X (%zd)    ", selName, numSel);
    }

    HMENU menu = GetMenu(wnd);
    MENUITEMINFO toolMenu = {sizeof(toolMenu), MIIM_SUBMENU};
    GetMenuItemInfo(menu, IDM_TOOL_MENU, false, &toolMenu);
    str += GetMenuString(toolMenu.hSubMenu, g_tool, str, 32, MF_BYPOSITION);
    if (TCHAR *tab = _tcsrchr(buf, L'\t')) str = tab;

    if (g_state.gridOn)
        str += _stprintf(str, L"    Grid: %f", g_state.gridSize);
    else
        str += _stprintf(str, L"    Grid: Off");

    MENUITEMINFO info = {sizeof(info), MIIM_STRING};
    info.dwTypeData = buf;
    SetMenuItemInfo(GetMenu(wnd), IDM_STATUS, false, &info);
    DrawMenuBar(wnd);
}

static void showError(HWND wnd, winged_error err) {
    if (err.message) {
        MessageBox(wnd, err.message, APP_NAME, MB_ICONERROR);
    } else {
        MessageBeep(MB_OK);
        SetCursor(LoadCursor(NULL, IDC_NO));
        Sleep(300);
    }
}

static void flashSel(HWND wnd) {
    g_flashSel = true;
    refreshImmediate(wnd);
    Sleep(200);
    g_flashSel = false;
    refresh(wnd);
}

static void lockMouse(HWND wnd, POINT clientPos, MouseMode mode) {
    if (!g_mouseMode) {
        SetCapture(wnd);
        ShowCursor(false);
    }
    g_curLockPos = clientToScreen(wnd, clientPos);
    g_mouseMode = mode;
}

static void setSelMode(SelectMode mode) {
    g_state.selMode = mode;
    if (!(tools[g_tool].allowedSelModes & (1 << mode)))
        g_tool = TOOL_SELECT;
}


static void CALLBACK tessVertexCallback(Vertex *vertex) {
    glVertex3fv(glm::value_ptr(vertex->pos));
}

static void CALLBACK tessErrorCallback(GLenum error) {
    g_tess_error = error;
}


static void updateProjMat() {
    glMatrixMode(GL_PROJECTION);
    float aspect = g_windowDim.x / g_windowDim.y;
    g_projMat = glm::perspective(glm::radians(g_view.flyCam ? 90.0f : 60.0f), aspect, 0.1f, 100.0f);
    glLoadMatrixf(glm::value_ptr(g_projMat));
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static BOOL onCreate(HWND wnd, LPCREATESTRUCT) {
    PIXELFORMATDESCRIPTOR formatDesc = {sizeof(formatDesc)};
    formatDesc.nVersion = 1;
    formatDesc.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    formatDesc.iPixelType = PFD_TYPE_RGBA;
    formatDesc.cColorBits = 24;
    formatDesc.cDepthBits = 32;
    formatDesc.iLayerType = PFD_MAIN_PLANE;

    HDC dc = GetDC(wnd);
    int pixelFormat = ChoosePixelFormat(dc, &formatDesc);
    SetPixelFormat(dc, pixelFormat, &formatDesc);
    HGLRC context = wglCreateContext(dc);
    if (!CHECKERR(context)) return false;
    CHECKERR(wglMakeCurrent(dc, context));

    glClearColor(.15f, .15f, .15f, 1.0f);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0, 1.0);

    g_tess = gluNewTess();
    gluTessCallback(g_tess, GLU_TESS_BEGIN, (GLvoid (CALLBACK*) ())glBegin);
    gluTessCallback(g_tess, GLU_TESS_END, (GLvoid (CALLBACK*) ())glEnd);
    gluTessCallback(g_tess, GLU_TESS_VERTEX, (GLvoid (CALLBACK*) ())tessVertexCallback);
    gluTessCallback(g_tess, GLU_TESS_ERROR, (GLvoid (CALLBACK*) ())tessErrorCallback);
    // gluTessCallback(g_tess, GLU_TESS_COMBINE, (GLvoid (*) ())tessCombineCallback);
    // gluTessCallback(g_tess, GLU_TESS_EDGE_FLAG, (GLvoid (*) ())glEdgeFlag);

    g_state.surf = makeCube();
    validateSurface(g_state.surf);

    updateStatus(wnd);
    return true;
}

static void onDestroy(HWND wnd) {
    gluDeleteTess(g_tess);
    if (HGLRC context = wglGetCurrentContext()) {
        HDC dc = wglGetCurrentDC();
        CHECKERR(wglMakeCurrent(NULL, NULL));
        ReleaseDC(wnd, dc);
        wglDeleteContext(context);
    }
}

static void onNCDestroy(HWND) {
    PostQuitMessage(0);
}

static EditorState knifeToVert(EditorState state, vert_id vert) {
    if (state.selVerts.size() == 1 && g_hoverFace.find(state.surf)) {
        edge_pair e1 = edgeOnHoverFace(state.surf, *state.selVerts.begin());
        edge_pair e2 = edgeOnHoverFace(state.surf, vert);

        if (e1.first == e2.first) {
            if (g_knifeVerts.empty())
                return state; // clicked same vertex twice, nothing to do
            // loop must be clockwise in this case
            glm::vec3 start = vert.in(state.surf).pos;
            glm::vec3 loopNorm = accumPolyNormal(start, g_knifeVerts[0]);
            for (int i = 1; i < g_knifeVerts.size(); i++)
                loopNorm += accumPolyNormal(g_knifeVerts[i - 1], g_knifeVerts[i]);
            loopNorm += accumPolyNormal(g_knifeVerts.back(), start);

            glm::vec3 faceNorm = faceNormalNonUnit(state.surf, g_hoverFace.in(state.surf));
            if (glm::dot(loopNorm, faceNorm) > 0)
                std::reverse(g_knifeVerts.begin(), g_knifeVerts.end());
        }

        edge_id newEdge;
        state.surf = splitFace(std::move(state.surf), e1.first, e2.first, g_knifeVerts, &newEdge);
        for (int i = 0; i < g_knifeVerts.size() + 1; i++) {
            edge_pair pair = newEdge.pair(state.surf);
            state.selEdges = std::move(state.selEdges).insert(primaryEdge(pair));
            newEdge = pair.second.next;
        }
    }

    state.selVerts = immer::set<vert_id>{}.insert(vert);
    g_knifeVerts.clear();
    return state;
}

static bool onSetCursor(HWND wnd, HWND cursorWnd, UINT hitTest, UINT msg) {
    if (msg && hitTest == HTCLIENT) {
        SetCursor(tools[g_tool].cursor);
        return true;
    }
    return FORWARD_WM_SETCURSOR(wnd, cursorWnd, hitTest, msg, DefWindowProc);
}

static EditorState join(EditorState state) {
    if (g_hover.vert.find(state.surf) && state.selVerts.size() == 1) {
        edge_id e1 = edgeOnHoverFace(state.surf, *state.selVerts.begin()).first;
        edge_id e2 = edgeOnHoverFace(state.surf, g_hover.vert).first;
        state.surf = mergeVerts(std::move(state.surf), e1, e2);
    } else if (auto hovEdge = g_hover.edge.find(state.surf)) {
        if (state.selEdges.size() != 1) throw winged_error();
        edge_pair edge1 = state.selEdges.begin()->pair(state.surf);
        edge_pair twin1 = edge1.second.twin.pair(state.surf);
        edge_pair edge2 = {g_hover.edge, *hovEdge};
        edge_pair twin2 = edge2.second.twin.pair(state.surf);
        if (edge1.first == edge2.first)
            throw winged_error();
        if (edge1.second.face == edge2.second.face) {} // do nothing
        else if (edge1.second.face == twin2.second.face) {
            std::swap(edge2, twin2);
        } else if (twin1.second.face == edge2.second.face) {
            std::swap(edge1, twin1);
        } else if (twin1.second.face == twin2.second.face) {
            std::swap(edge1, twin1); std::swap(edge2, twin2);
        } else {
            throw winged_error(); // edges don't share a face
        }

        if (edge2.second.next != edge1.first)
            state.surf = mergeVerts(std::move(state.surf), edge1.first, edge2.second.next);
        if (edge1.second.next != edge2.first)
            state.surf = mergeVerts(std::move(state.surf), edge1.second.next, edge2.first);
    } else if (auto face2 = g_hover.face.find(state.surf)) {
        if (state.selFaces.size() != 1) throw winged_error();
        Face face1 = state.selFaces.begin()->in(state.surf);
        edge_id e1, e2;
        std::tie(e1, e2) = findClosestOpposingEdges(state.surf, face1, *face2);
        state.surf = joinEdgeLoops(std::move(state.surf), e1, e2);
        // TODO: select edge loop
    } else {
        throw winged_error();
    }
    return state;
}

static void onLButtonDown(HWND wnd, BOOL, int x, int y, UINT) {
    if (g_tool == TOOL_KNIFE) {
        try {
            switch (hoverType()) {
                case Surface::EDGE: {
                    EditorState newState = g_state;
                    newState.surf = splitEdge(g_state.surf, g_hover.edge, g_hover.point);
                    vert_id newVert = g_hover.edge.in(newState.surf).next.in(newState.surf).vert;
                    newState = knifeToVert(std::move(newState), newVert);
                    pushUndo(std::move(newState));
                    break;
                }
                case Surface::VERT:
                    pushUndo(knifeToVert(g_state, g_hover.vert));
                    break;
                case Surface::FACE:
                    g_knifeVerts.push_back(g_hover.point);
                    break;
                case Surface::NONE:
                    g_state = clearSelection(std::move(g_state));
                    break;
            }
        } catch (winged_error err) {
            showError(wnd, err);
        }
    } else if (g_tool == TOOL_JOIN && hasSelection(g_state) && g_hover.type) {
        try {
            pushUndo(join(g_state));
            flashSel(wnd);
            if (!(GetKeyState(VK_SHIFT) < 0))
                g_tool = TOOL_SELECT;
        } catch (winged_error err) {
            showError(wnd, err);
        }
    } else {
        if (!hasSelection(g_state)) {
            g_state = select(std::move(g_state), g_hover.type, g_hover.id);
            refreshImmediate(wnd);
        }
        if (DragDetect(wnd, clientToScreen(wnd, {x, y}))) {
            if (hasSelection(g_state) && (g_tool == TOOL_SELECT || g_tool == TOOL_SCALE))
                pushUndo();
            lockMouse(wnd, {x, y}, MOUSE_ADJUST);
            g_moveAccum = {};
        } else {
            if (!(GetKeyState(VK_SHIFT) < 0))
                g_state = clearSelection(std::move(g_state));
            g_state = select(std::move(g_state), g_hover.type, g_hover.id);
        }
    }
    updateStatus(wnd);
    refresh(wnd);
}

static void onRButtonDown(HWND wnd, BOOL, int x, int y, UINT) {
    lockMouse(wnd, {x, y}, MOUSE_CAM_ROTATE);
}

static void onMButtonDown(HWND wnd, BOOL, int x, int y, UINT) {
    lockMouse(wnd, {x, y}, MOUSE_CAM_PAN);
}

static void onButtonUp(HWND, int, int, UINT) {
    if (g_mouseMode) {
        g_mouseMode = MOUSE_NONE;
        ReleaseCapture();
        ShowCursor(true);
    }
}

static glm::vec3 snapAxis(glm::vec3 v) {
    int axis = maxAxis(glm::abs(v));
    glm::vec3 dir = {};
    dir[axis] = v[axis] >= 0 ? 1.0f : -1.0f;
    return dir;
}

static void toolAdjust(HWND, SIZE delta, UINT) {
    switch (g_tool) {
        case TOOL_SELECT: {
            glm::vec2 scaled = glm::vec2(delta.cx, delta.cy) * g_view.zoom / 600.0f;
            if (g_state.gridOn) {
                g_moveAccum = glm::modf(g_moveAccum + (scaled / g_state.gridSize), scaled);
                scaled *= g_state.gridSize;
            }

            glm::mat4 invMV = glm::inverse(g_mvMat);
            glm::vec3 right = {}, down = {};
            if (GetKeyState(VK_CONTROL) < 0) {
                if (g_state.selFaces.size() == 1) { // TODO: move along each face's normal?
                    down = faceNormal(g_state.surf, g_state.selFaces.begin()->in(g_state.surf));
                    if (g_state.gridOn)
                        down /= down[maxAxis(down)]; // scale to cartesian grid
                }
            } else if (GetKeyState(VK_SHIFT) < 0 && GetKeyState(VK_MENU) < 0)
                down = snapAxis(invMV[2]);
            else if (GetKeyState(VK_SHIFT) < 0) down = snapAxis(-invMV[1]);
            else if (GetKeyState(VK_MENU) < 0) right = snapAxis(invMV[0]);
            else { right = snapAxis(invMV[0]), down = snapAxis(-invMV[1]); }

            glm::vec3 deltaPos = right * scaled.x + down * scaled.y;
            for (auto v : selAttachedVerts(g_state))
                g_state.surf = moveVertex(std::move(g_state.surf), v, deltaPos);
            break;
        }
        case TOOL_SCALE: {
            glm::vec3 factor = glm::vec3(glm::pow(1.001f, (float)delta.cx));
            auto verts = selAttachedVerts(g_state);
            glm::vec3 center = {};
            for (auto v : verts)
                center += v.in(g_state.surf).pos;
            center /= verts.size();
            for (auto v : verts)
                g_state.surf = scaleVertex(std::move(g_state.surf), v, center, factor);
            break;
        }
    }
}

static void onMouseMove(HWND wnd, int x, int y, UINT keyFlags) {
    if (g_mouseMode == MOUSE_NONE) {
        bool elementSel = g_state.selMode == SEL_ELEMENTS;
        auto result = pickElement(g_state.surf, elementSel ? Surface::ALL : Surface::FACE,
            {x, y}, g_windowDim, g_projMat * g_mvMat,
            (g_state.gridOn && g_tool == TOOL_KNIFE && elementSel) ? g_state.gridSize : 0);
        if (result.id != g_hover.id || result.point != g_hover.point) {
            g_hover = result;
            if (result.type == Surface::FACE)
                g_hoverFace = g_hover.face;
            refresh(wnd);
            if (g_tool == TOOL_KNIFE)
                updateStatus(wnd);
        }
        return;
    }

    POINT screenPos = clientToScreen(wnd, {x, y});
    if (screenPos == g_curLockPos || g_mouseMode == MOUSE_NONE)
        return;
    SIZE delta = {screenPos.x - g_curLockPos.x, screenPos.y - g_curLockPos.y};
    switch (g_mouseMode) {
        case MOUSE_ADJUST:
            toolAdjust(wnd, delta, keyFlags);
            break;
        case MOUSE_CAM_ROTATE:
            g_view.rotX += glm::radians((float)delta.cy) * 0.5f;
            g_view.rotY += glm::radians((float)delta.cx) * 0.5f;
            break;
        case MOUSE_CAM_PAN: {
            bool shift = GetKeyState(VK_SHIFT) < 0;
            glm::vec3 deltaPos;
            if (g_view.flyCam) {
                deltaPos = shift ? glm::vec3(-delta.cx, delta.cy, 0)
                    : glm::vec3(-delta.cx, 0, -delta.cy);
            } else {
                deltaPos = shift ? glm::vec3(0, 0, -delta.cy) : glm::vec3(delta.cx, -delta.cy, 0);
            }
            deltaPos = glm::inverse(g_mvMat) * glm::vec4(deltaPos, 0);
            g_view.camPivot += deltaPos * g_view.zoom / 600.0f;
        }
    }
    refresh(wnd);
    SetCursorPos(g_curLockPos.x, g_curLockPos.y);
}

void onMouseWheel(HWND wnd, int, int, int delta, UINT) {
    g_view.zoom *= glm::pow(1.001f, g_view.flyCam ? delta : -delta);
    refresh(wnd);
}

static void saveAs(HWND wnd) {
    TCHAR fileName[MAX_PATH];
    fileName[0] = 0;
    const TCHAR filters[] = L"WingEd File (.wing)\0*.wing\0All Files\0*.*\0\0";
    if (GetSaveFileName(tempPtr(makeOpenFileName(fileName, wnd, filters, L"wing")))) {
        writeFile(fileName, g_state, g_view);
        memcpy(g_fileName, fileName, sizeof(g_fileName));
    }
}

static EditorState erase(EditorState state) {
    EditorState newState = state;
    if (state.selMode == SEL_ELEMENTS) {
        // edges first, then vertices
        bool anyDeleted = false;
        for (auto &e : state.selEdges) {
            if (e.find(newState.surf)) { // could have been deleted previously
                newState.surf = mergeFaces(std::move(newState.surf), e);
                anyDeleted = true;
            }
        }
        for (auto &v : state.selVerts) {
            if (auto vert = v.find(newState.surf)) {
                // make sure vert has only two edges
                const HEdge &edge = vert->edge.in(newState.surf);
                const HEdge &twin = edge.twin.in(newState.surf);
                const HEdge &twinNext = twin.next.in(newState.surf);
                if (twinNext.twin.in(newState.surf).next == vert->edge) {
                    newState.surf = mergeVerts(std::move(newState.surf),
                        edge.prev, vert->edge);
                    anyDeleted = true;
                }
            }
        }
        if (!anyDeleted)
            throw winged_error();
    } else if (state.selMode == SEL_SOLIDS) {
        for (auto &v : state.selVerts)
            newState.surf.verts = std::move(newState.surf.verts).erase(v);
        for (auto &f : state.selFaces)
            newState.surf.faces = std::move(newState.surf.faces).erase(f);
        for (auto &e : state.selEdges)
            newState.surf.edges = std::move(newState.surf.edges).erase(e)
                .erase(e.in(state.surf).twin);
    } else {
        throw winged_error();
    }
    return newState;
}

static void onCommand(HWND wnd, int id, HWND ctl, UINT) {
    if (ctl) return;

    try {
        switch (id) {
            /* File */
            case IDM_OPEN: {
                TCHAR fileName[MAX_PATH];
                fileName[0] = 0;
                const TCHAR filters[] = L"WingEd File (.wing)\0*.wing\0\0";
                if (GetOpenFileName(tempPtr(makeOpenFileName(fileName, wnd, filters, L"wing")))) {
                    std::tie(g_state, g_view) = readFile(fileName);
                    g_undoStack = {};
                    g_redoStack = {};
                    memcpy(g_fileName, fileName, sizeof(g_fileName));
                }
                break;
            }
            case IDM_SAVE_AS:
                saveAs(wnd);
                break;
            case IDM_SAVE:
                if (!g_fileName[0])
                    saveAs(wnd);
                else
                    writeFile(g_fileName, g_state, g_view);
                break;
            /* Tool */
            case IDM_TOOL_SELECT:
                g_tool = TOOL_SELECT;
                break;
            case IDM_TOOL_SCALE:
                g_tool = TOOL_SCALE;
                break;
            case IDM_TOOL_KNIFE:
                g_tool = TOOL_KNIFE;
                g_knifeVerts.clear();
                break;
            case IDM_TOOL_JOIN:
                g_tool = TOOL_JOIN;
                break;
            /* Select */
            case IDM_CLEAR_SELECT:
                g_state = clearSelection(std::move(g_state));
                g_knifeVerts.clear();
                break;
            case IDM_SEL_ELEMENTS:
                setSelMode(SEL_ELEMENTS);
                break;
            case IDM_SEL_SOLIDS:
                if (g_state.selMode != SEL_SOLIDS) {
                    g_state = clearSelection(std::move(g_state));
                    g_hoverFace = {};
                    g_hover.type = Surface::NONE;
                }
                setSelMode(SEL_SOLIDS);
                break;
            case IDM_EDGE_TWIN:
                g_state.selEdges = immer::set<edge_id>{}.insert(expectSingleSelEdge().twin);
                break;
            case IDM_NEXT_FACE_EDGE:
                g_state.selEdges = immer::set<edge_id>{}.insert(expectSingleSelEdge().next);
                break;
            case IDM_PREV_FACE_EDGE:
                g_state.selEdges = immer::set<edge_id>{}.insert(expectSingleSelEdge().prev);
                break;
            /* View */
            case IDM_FLY_CAM:
                g_view.flyCam ^= true;
                updateProjMat();
                break;
            /* Edit */
            case IDM_UNDO:
                if (!g_undoStack.empty()) {
                    g_redoStack.push(g_state);
                    g_state = g_undoStack.top();
                    g_undoStack.pop();
                }
                g_knifeVerts.clear();
                break;
            case IDM_REDO:
                if (!g_redoStack.empty()) {
                    g_undoStack.push(g_state);
                    g_state = g_redoStack.top();
                    g_redoStack.pop();
                }
                g_knifeVerts.clear();
                break;
            case IDM_TOGGLE_GRID:
                g_state.gridOn ^= true;
                break;
            case IDM_GRID_DOUBLE:
                g_state.gridSize *= 2;
                break;
            case IDM_GRID_HALF:
                g_state.gridSize /= 2;
                break;
            case IDM_KNIFE_BKSP:
                if (!g_knifeVerts.empty())
                    g_knifeVerts.pop_back();
                break;
            /* undoable operations... */
            case IDM_ERASE:
                pushUndo(erase(g_state));
                break;
            /* element */
            case IDM_EXTRUDE: {
                EditorState newState = g_state;
                for (auto f : g_state.selFaces)
                    newState.surf = extrudeFace(newState.surf, f);
                pushUndo(std::move(newState));
                flashSel(wnd);
                break;
            }
            case IDM_SPLIT_LOOP: {
                auto loop = sortEdgeLoop(g_state.surf, g_state.selEdges);
                EditorState newState = g_state;
                newState.surf = splitEdgeLoop(g_state.surf, loop);
                newState.selVerts = {};
                newState.selEdges = {};
                for (auto &e : loop) {
                    edge_id primary = primaryEdge(e.pair(newState.surf));
                    newState.selEdges = std::move(newState.selEdges).insert(primary);
                }
                pushUndo(std::move(newState));
                flashSel(wnd);
                break;
            }
            /* solid */
            case IDM_FLIP_NORMALS: {
                EditorState newState = g_state;
                newState.surf = flipNormals(g_state.surf, g_state.selEdges, g_state.selVerts);
                pushUndo(std::move(newState));
                break;
            }
        }
    } catch (winged_error err) {
        showError(wnd, err);
    }
    updateStatus(wnd);
    refresh(wnd);
}

static void onInitMenu(HWND, HMENU menu) {
    bool hasSel = hasSelection(g_state);
    bool selElem = g_state.selMode == SEL_ELEMENTS;
    bool selSolid = g_state.selMode == SEL_SOLIDS;
    EnableMenuItem(menu, IDM_CLEAR_SELECT, hasSel ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_UNDO, g_undoStack.empty() ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(menu, IDM_REDO, g_redoStack.empty() ? MF_GRAYED : MF_ENABLED);
    CheckMenuItem(menu, IDM_TOGGLE_GRID, g_state.gridOn ? MF_CHECKED : MF_UNCHECKED);
    EnableMenuItem(menu, IDM_ERASE, hasSel ? MF_ENABLED : MF_DISABLED);
    EnableMenuItem(menu, IDM_EXTRUDE, (!g_state.selFaces.empty() && selElem) ?
        MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_SPLIT_LOOP, (!g_state.selEdges.empty() && selElem) ?
        MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_FLIP_NORMALS, (hasSel && selSolid) ? MF_ENABLED : MF_GRAYED);
    CheckMenuItem(menu, IDM_FLY_CAM, g_view.flyCam ? MF_CHECKED : MF_UNCHECKED);

    MENUITEMINFO selMenu = {sizeof(selMenu), MIIM_SUBMENU};
    GetMenuItemInfo(menu, IDM_SEL_MENU, false, &selMenu);
    CheckMenuRadioItem(selMenu.hSubMenu, 0, NUM_SELMODES - 1, g_state.selMode, MF_BYPOSITION);

    MENUITEMINFO toolMenu = {sizeof(toolMenu), MIIM_SUBMENU};
    GetMenuItemInfo(menu, IDM_TOOL_MENU, false, &toolMenu);
    CheckMenuRadioItem(toolMenu.hSubMenu, 0, NUM_TOOLS - 1, g_tool, MF_BYPOSITION);
    for (int i = 0; i < NUM_TOOLS; i++) {
        EnableMenuItem(toolMenu.hSubMenu, GetMenuItemID(toolMenu.hSubMenu, i),
            (tools[i].allowedSelModes & (1 << g_state.selMode)) ? MF_ENABLED : MF_GRAYED);
    }
}

static void onSize(HWND, UINT, int cx, int cy) {
    g_windowDim = {cx, cy};
    glViewport(0, 0, cx, cy);
    updateProjMat();
}

static void drawFace(const Surface &surf, const Face &face) {
    // TODO cache faces
    // https://www.glprogramming.com/red/chapter11.html
    g_tess_error = 0;
    glm::vec3 normal = faceNormal(surf, face);
    gluTessNormal(g_tess, normal.x, normal.y, normal.z);
    gluTessBeginPolygon(g_tess, NULL);
    gluTessBeginContour(g_tess);
    for (auto &ep : FaceEdges(surf, face)) {
        const Vertex &vert = ep.second.vert.in(surf);
        glm::dvec3 dPos = vert.pos;
        gluTessVertex(g_tess, glm::value_ptr(dPos), (void *)&vert);
    }
    gluTessEndContour(g_tess);
    gluTessEndPolygon(g_tess);
    if (g_tess_error) {
        // fallback
        glColor3f(1, 0, 0);
        glBegin(GL_TRIANGLE_FAN);
        for (auto &ep : FaceEdges(surf, face)) {
            glVertex3fv(glm::value_ptr(ep.second.vert.in(surf).pos));
        }
        glEnd();
    }
}

static void drawEdge(const Surface &surf, const HEdge &edge) {
    glVertex3fv(glm::value_ptr(edge.vert.in(surf).pos));
    glVertex3fv(glm::value_ptr(edge.twin.in(surf).vert.in(surf).pos));
}

static void drawState(const EditorState &state) {
    glLineWidth(1);
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    for (auto &pair : state.surf.edges) {
        if (isPrimary(pair))
            drawEdge(state.surf, pair.second);
    }
    glEnd();

    if (g_state.selMode == SEL_ELEMENTS) {
        glLineWidth(5);
        if (g_flashSel)
            glColor3f(1, 0, 0);
        else
            glColor3f(1, 1, 0);
        glBegin(GL_LINES);
        for (auto e : state.selEdges) {
            drawEdge(state.surf, e.in(state.surf));
        }
        glEnd();
        if (auto hoverEdge = g_hover.edge.find(state.surf)) {
            glLineWidth(3);
            glColor3f(1, 0.3f, 0.5f);
            glBegin(GL_LINES);
            drawEdge(state.surf, *hoverEdge);
            glEnd();
        }

        glPointSize(7);
        glBegin(GL_POINTS);
        for (auto &pair : state.surf.verts) {
            if (state.selVerts.count(pair.first)) {
                if (g_flashSel)
                    glColor3f(1, 0.8f, 0.8f);
                else
                    glColor3f(1, 0, 0);
            } else {
                glColor3f(0, 1, 0);
            }
            glVertex3fv(glm::value_ptr(pair.second.pos));
        }
        if (g_tool == TOOL_KNIFE) {
            glColor3f(1, 1, 1);
            if (state.selVerts.size() == 1) {
                for (auto &v : g_knifeVerts)
                    glVertex3fv(glm::value_ptr(v));
            }
            if (g_hover.type && g_hover.type != Surface::VERT)
                glVertex3fv(glm::value_ptr(g_hover.point));
        }
        glEnd();
        if (auto hoverVert = g_hover.vert.find(state.surf)) {
            glPointSize(11);
            glColor3f(1, 1, 1);
            glBegin(GL_POINTS);
            glVertex3fv(glm::value_ptr(hoverVert->pos));
            glEnd();
        }

        if (g_tool == TOOL_KNIFE && state.selVerts.size() == 1
                && (g_hover.type || !g_knifeVerts.empty())) {
            glColor3f(1, 1, 1);
            glLineWidth(1);
            glBegin(GL_LINE_STRIP);
            glVertex3fv(glm::value_ptr(state.selVerts.begin()->in(state.surf).pos));
            for (auto &v : g_knifeVerts)
                glVertex3fv(glm::value_ptr(v));
            if (g_hover.type)
                glVertex3fv(glm::value_ptr(g_hover.point));
            glEnd();
        }
    }

    for (auto &pair : state.surf.faces) {
        if (state.selFaces.count(pair.first)) {
            if (g_flashSel)
                glColor3f(0, 1, 0.5);
            else
                glColor3f(0, 0.5, 1);
        } else if (g_hover.type && pair.first == g_hoverFace
                && (g_hover.type == Surface::FACE || g_tool == TOOL_KNIFE || g_tool == TOOL_JOIN)) {
            glColor3f(0.25, 0.25, 1);
        } else {
            glColor3f(0, 0, 1);
        }
        drawFace(state.surf, pair.second);
    }
}

static void onPaint(HWND wnd) {
    PAINTSTRUCT ps;
    BeginPaint(wnd, &ps);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    g_mvMat = glm::mat4(1);
    if (!g_view.flyCam)
        g_mvMat = glm::translate(g_mvMat, glm::vec3(0, 0, -g_view.zoom));
    g_mvMat = glm::rotate(g_mvMat, g_view.rotX, glm::vec3(1, 0, 0));
    g_mvMat = glm::rotate(g_mvMat, g_view.rotY, glm::vec3(0, 1, 0));
    g_mvMat = glm::translate(g_mvMat, g_view.camPivot);
    glLoadMatrixf(glm::value_ptr(g_mvMat));

    drawState(g_state);

    SwapBuffers(ps.hdc);
    EndPaint(wnd, &ps);
}

static LRESULT CALLBACK MainWindowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(wnd, WM_CREATE, onCreate);
        HANDLE_MSG(wnd, WM_DESTROY, onDestroy);
        HANDLE_MSG(wnd, WM_NCDESTROY, onNCDestroy);
        HANDLE_MSG(wnd, WM_SETCURSOR, onSetCursor);
        HANDLE_MSG(wnd, WM_LBUTTONDOWN, onLButtonDown);
        HANDLE_MSG(wnd, WM_RBUTTONDOWN, onRButtonDown);
        HANDLE_MSG(wnd, WM_MBUTTONDOWN, onMButtonDown);
        HANDLE_MSG(wnd, WM_LBUTTONUP, onButtonUp);
        HANDLE_MSG(wnd, WM_RBUTTONUP, onButtonUp);
        HANDLE_MSG(wnd, WM_MBUTTONUP, onButtonUp);
        HANDLE_MSG(wnd, WM_MOUSEMOVE, onMouseMove);
        HANDLE_MSG(wnd, WM_MOUSEWHEEL, onMouseWheel);
        HANDLE_MSG(wnd, WM_COMMAND, onCommand);
        HANDLE_MSG(wnd, WM_INITMENU, onInitMenu);
        HANDLE_MSG(wnd, WM_SIZE, onSize);
        HANDLE_MSG(wnd, WM_PAINT, onPaint);
    }
    return DefWindowProc(wnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCmd) {
    WNDCLASSEX wndClass = makeClass(APP_NAME, MainWindowProc);
    wndClass.style = CS_HREDRAW | CS_VREDRAW;
    wndClass.lpszMenuName = APP_NAME;
    wndClass.hCursor = NULL;
    RegisterClassEx(&wndClass);
    HWND wnd = createWindow(APP_NAME, APP_NAME,
        defaultWindowRect(640, 480, WS_OVERLAPPEDWINDOW, true));
    if (!wnd) return -1;
    ShowWindow(wnd, showCmd);
    return simpleMessageLoop(wnd, LoadAccelerators(instance, L"Accel"));
}

CHROMA_MAIN
