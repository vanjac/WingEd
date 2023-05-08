#include "common.h"
#include "winchroma.h"

#include <stack>
#include <gl/GL.h>
#include <gl/GLU.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/associated_min_max.hpp>
#include "surface.h"
#include "ops.h"
#include "picking.h"
#include "resource.h"

#pragma comment(lib, "Rpcrt4.lib")
#pragma comment(lib, "Opengl32.lib")
#pragma comment(lib, "Glu32.lib")

using namespace chroma;
using namespace winged;

const TCHAR APP_NAME[] = _T("WingEd");
enum Tool {
    TOOL_SELECT, TOOL_SCALE, TOOL_KNIFE, NUM_TOOLS
};
const TCHAR * const toolNames[] = {L"Select", L"Scale", L"Knife"};
const UINT toolCommands[] = {IDM_TOOL_SELECT, IDM_TOOL_SCALE, IDM_TOOL_KNIFE};
enum MouseMode {
    MOUSE_NONE = 0, MOUSE_ADJUST, MOUSE_CAM_ROTATE, MOUSE_CAM_PAN
};


struct EditorState {
    Surface surf;
    Surface::ElementType selType;
    union {
        id_t sel;
        vert_id selVert;
        face_id selFace;
        edge_id selEdge;
    };
    bool gridOn = true;
    float gridSize = 0.25f;
};
static EditorState g_state;
static std::stack<EditorState> g_undoStack;
static std::stack<EditorState> g_redoStack;

static Tool g_tool = TOOL_SELECT;
static PickResult g_hover;
static face_id g_hoverFace;
static MouseMode g_mouseMode = MOUSE_NONE;
static POINT g_curLockPos;
static glm::vec3 g_moveAccum;
static bool g_flashSel = false;

static glm::vec3 g_camPivot = {};
static float g_rotX = 0, g_rotY = 0;
static float g_zoom = 4;
static bool g_flyCam = false;
static glm::mat4 g_projMat, g_mvMat;
static glm::vec2 g_windowDim;

static GLUtesselator *g_tess;
static GLenum g_tess_error;


static void pushUndo() {
    g_undoStack.push(g_state);
    g_redoStack = {};
}

static void pushUndo(const EditorState &newState) {
    validateSurface(newState.surf);
    pushUndo();
    g_state = newState;
}

static bool anySelection() {
    switch (g_state.selType) {
        case Surface::VERT: return g_state.selVert.find(g_state.surf);
        case Surface::FACE: return g_state.selFace.find(g_state.surf);
        case Surface::EDGE: return g_state.selEdge.find(g_state.surf);
        default: return false;
    }
}

static bool anyHover() {
    switch (g_hover.type) {
        case Surface::VERT: return g_hover.vert.find(g_state.surf);
        case Surface::FACE: return g_hover.face.find(g_state.surf);
        case Surface::EDGE: return g_hover.edge.find(g_state.surf);
        default: return false;
    }
}

static const HEdge expectSelEdge() {
    if (auto sel = g_state.selEdge.find(g_state.surf))
        return *sel;
    throw winged_error(L"No selected edge");
}

static void expectSelFace() {
    if (!g_state.selFace.find(g_state.surf))
        throw winged_error(L"No selected face");
}

static void expectHoverVert() {
    if (!g_hover.type || !g_hover.vert.find(g_state.surf))
        throw winged_error();
}

static void expectHoverEdge() {
    if (!g_hover.type || !g_hover.edge.find(g_state.surf))
        throw winged_error();
}

static edge_pair edgeOnHoverFace(const Surface &surf, vert_id v) {
    for (auto &edge : VertEdges(surf, v.in(surf))) {
        if (edge.second.face == g_hoverFace)
            return edge;
    }
    throw winged_error();
}


static void refresh(HWND wnd) {
    InvalidateRect(wnd, NULL, false);
}

static void refreshImmediate(HWND wnd) {
    RedrawWindow(wnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void updateStatus(HWND wnd) {
    TCHAR buf[64], gridBuf[16];
    if (g_state.gridOn)
        _stprintf_s(gridBuf, _countof(gridBuf), L"%f", g_state.gridSize);
    else
        _stprintf_s(gridBuf, _countof(gridBuf), L"Off");
    _stprintf_s(buf, _countof(buf), L"%s  \t\t%s \tGrid: %s",
        APP_NAME, toolNames[g_tool], gridBuf);
    SetWindowText(wnd, buf);
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


static void tessVertexCallback(Vertex *vertex) {
    glVertex3fv(glm::value_ptr(vertex->pos));
}

static void tessErrorCallback(GLenum error) {
    g_tess_error = error;
}


static void updateProjMat() {
    glMatrixMode(GL_PROJECTION);
    float aspect = g_windowDim.x / g_windowDim.y;
    g_projMat = glm::perspective(glm::radians(g_flyCam ? 90.0f : 60.0f), aspect, 0.1f, 100.0f);
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
    gluTessCallback(g_tess, GLU_TESS_BEGIN, (GLvoid (*) ())glBegin);
    gluTessCallback(g_tess, GLU_TESS_END, (GLvoid (*) ())glEnd);
    gluTessCallback(g_tess, GLU_TESS_VERTEX, (GLvoid (*) ())tessVertexCallback);
    gluTessCallback(g_tess, GLU_TESS_ERROR, (GLvoid (*) ())tessErrorCallback);
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
    if (state.selVert.find(state.surf) && g_hoverFace.find(state.surf)) {
        edge_pair e1 = edgeOnHoverFace(state.surf, state.selVert);
        edge_pair e2 = edgeOnHoverFace(state.surf, vert);
        state.surf = splitFace(std::move(state.surf), e1.first, e2.first);
    }
    state.selType = Surface::VERT;
    state.selVert = vert;
    return state;
}

static void onLButtonDown(HWND wnd, BOOL, int x, int y, UINT) {
    if (g_tool == TOOL_KNIFE) {
        try {
            switch (g_hover.type) {
                case Surface::EDGE: {
                    EditorState newState = g_state;
                    newState.surf = splitEdge(g_state.surf, g_hover.edge, g_hover.point);
                    vert_id newVert = g_hover.edge.in(newState.surf).next.in(newState.surf).vert;
                    newState = knifeToVert(std::move(newState), newVert);
                    pushUndo(newState);
                    break;
                }
                case Surface::VERT:
                    pushUndo(knifeToVert(g_state, g_hover.vert));
                    break;
                case Surface::FACE:
                    throw winged_error();
                    break;
                case Surface::NONE:
                    g_state.selType = Surface::NONE;
                    g_state.sel = {};
                    break;
            }
        } catch (winged_error err) {
            showError(wnd, err);
        }
    } else {
        if (!anySelection()) {
            g_state.selType = g_hover.type;
            g_state.sel = g_hover.id;
            refreshImmediate(wnd);
        }
        if (DragDetect(wnd, clientToScreen(wnd, {x, y}))) {
            if (g_tool == TOOL_SELECT || g_tool == TOOL_SCALE)
                pushUndo();
            lockMouse(wnd, {x, y}, MOUSE_ADJUST);
            g_moveAccum = {};
        } else {
            g_state.selType = g_hover.type;
            g_state.sel = g_hover.id;
        }
    }
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
    glm::vec3 absV = glm::abs(v);
    int axis = glm::associatedMax(absV.x, 0, absV.y, 1, absV.z, 2);
    glm::vec3 dir = {};
    dir[axis] = v[axis] >= 0 ? 1.0f : -1.0f;
    return dir;
}

static void toolAdjust(HWND, SIZE delta, UINT) {
    switch (g_tool) {
        case TOOL_SELECT: {
            glm::mat4 invMV = glm::inverse(g_mvMat);
            glm::vec3 right = {}, down = {};
            if (GetKeyState(VK_CONTROL) < 0) {
                if (auto face = g_state.selFace.find(g_state.surf))
                    down = faceNormal(g_state.surf, *face);
            } else if (GetKeyState(VK_SHIFT) < 0 && GetKeyState(VK_MENU) < 0)
                down = snapAxis(invMV[2]);
            else if (GetKeyState(VK_SHIFT) < 0) down = snapAxis(-invMV[1]);
            else if (GetKeyState(VK_MENU) < 0) right = snapAxis(invMV[0]);
            else { right = snapAxis(invMV[0]), down = snapAxis(-invMV[1]); }
            glm::vec3 deltaPos = right * (float)delta.cx + down * (float)delta.cy;
            deltaPos *= g_zoom / 600.0f;
            glm::vec3 adjustAmt;
            if (g_state.gridOn) {
                g_moveAccum = glm::modf(g_moveAccum + (deltaPos / g_state.gridSize), adjustAmt);
                adjustAmt *= g_state.gridSize;
            } else {
                adjustAmt = deltaPos;
            }
            if (g_state.selType == Surface::VERT && g_state.selVert.find(g_state.surf)) {
                g_state.surf = moveVertex(g_state.surf, g_state.selVert, adjustAmt);
            } else if (g_state.selType == Surface::EDGE) {
                if (auto edge = g_state.selEdge.find(g_state.surf)) {
                    g_state.surf = moveVertex(g_state.surf, edge->vert, adjustAmt);
                    vert_id twinVert = edge->twin.in(g_state.surf).vert;
                    g_state.surf = moveVertex(g_state.surf, twinVert, adjustAmt);
                }
            } else if (g_state.selType == Surface::FACE) {
                if (auto face = g_state.selFace.find(g_state.surf)) {
                    for (auto &faceEdge : FaceEdges(g_state.surf, *face))
                        g_state.surf = moveVertex(g_state.surf, faceEdge.second.vert, adjustAmt);
                }
            }
            break;
        }
        case TOOL_SCALE: {
            glm::vec3 factor = glm::vec3(glm::pow(1.001f, (float)delta.cx));
            if (g_state.selType == Surface::EDGE) {
                if (auto edge = g_state.selEdge.find(g_state.surf)) {
                    vert_id twinVert = edge->twin.in(g_state.surf).vert;
                    glm::vec3 center = edge->vert.in(g_state.surf).pos;
                    center = (center + twinVert.in(g_state.surf).pos) / 2.0f;
                    g_state.surf = scaleVertex(g_state.surf, edge->vert, center, factor);
                    g_state.surf = scaleVertex(g_state.surf, twinVert, center, factor);
                }
            } else if (g_state.selType == Surface::FACE) {
                if (auto face = g_state.selFace.find(g_state.surf)) {
                    glm::vec3 center = {};
                    int count = 0;
                    for (auto &faceEdge : FaceEdges(g_state.surf, *face)) {
                        center += faceEdge.second.vert.in(g_state.surf).pos;
                        count++;
                    }
                    center /= count;
                    for (auto &faceEdge : FaceEdges(g_state.surf, *face)) {
                        g_state.surf = scaleVertex(g_state.surf, faceEdge.second.vert,
                            center, factor);
                    }
                }
            }
            break;
        }
    }
}

static void onMouseMove(HWND wnd, int x, int y, UINT keyFlags) {
    if (g_mouseMode == MOUSE_NONE) {
        auto result = pickElement(g_state.surf, Surface::ALL,
            {x, y}, g_windowDim, g_projMat * g_mvMat,
            (g_state.gridOn && g_tool == TOOL_KNIFE) ? g_state.gridSize : 0);
        if (result.id != g_hover.id || result.point != g_hover.point) {
            g_hover = result;
            if (g_hover.type == Surface::FACE)
                g_hoverFace = g_hover.face;
            refresh(wnd);
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
            g_rotX += glm::radians((float)delta.cy) * 0.5f;
            g_rotY += glm::radians((float)delta.cx) * 0.5f;
            break;
        case MOUSE_CAM_PAN: {
            bool shift = GetKeyState(VK_SHIFT) < 0;
            glm::vec3 deltaPos;
            if (g_flyCam) {
                deltaPos = shift ? glm::vec3(-delta.cx, delta.cy, 0)
                    : glm::vec3(-delta.cx, 0, -delta.cy);
            } else {
                deltaPos = shift ? glm::vec3(0, 0, -delta.cy) : glm::vec3(delta.cx, -delta.cy, 0);
            }
            deltaPos = glm::inverse(g_mvMat) * glm::vec4(deltaPos, 0);
            g_camPivot += deltaPos * g_zoom / 600.0f;
        }
    }
    refresh(wnd);
    SetCursorPos(g_curLockPos.x, g_curLockPos.y);
}

void onMouseWheel(HWND wnd, int, int, int delta, UINT) {
    g_zoom *= glm::pow(1.001f, g_flyCam ? delta : -delta);
    refresh(wnd);
}

static void onCommand(HWND wnd, int id, HWND ctl, UINT) {
    if (ctl) return;

    try {
        switch (id) {
            case IDM_UNDO:
                if (!g_undoStack.empty()) {
                    g_redoStack.push(g_state);
                    g_state = g_undoStack.top();
                    g_undoStack.pop();
                    updateStatus(wnd);
                }
                break;
            case IDM_REDO:
                if (!g_redoStack.empty()) {
                    g_undoStack.push(g_state);
                    g_state = g_redoStack.top();
                    g_redoStack.pop();
                    updateStatus(wnd);
                }
                break;
            /* tools */
            case IDM_TOOL_SELECT:
                g_tool = TOOL_SELECT;
                updateStatus(wnd);
                break;
            case IDM_TOOL_SCALE:
                g_tool = TOOL_SCALE;
                updateStatus(wnd);
                break;
            case IDM_TOOL_KNIFE:
                g_tool = TOOL_KNIFE;
                updateStatus(wnd);
                break;
            /* navigation */
            case IDM_CLEAR_SELECT:
                g_state.selType = Surface::NONE;
                g_state.sel = {};
                break;
            case IDM_EDGE_TWIN:
                g_state.selEdge = expectSelEdge().twin;
                break;
            case IDM_NEXT_FACE_EDGE:
                g_state.selEdge = expectSelEdge().next;
                break;
            case IDM_PREV_FACE_EDGE:
                g_state.selEdge = expectSelEdge().prev;
                break;
            case IDM_TOGGLE_GRID:
                g_state.gridOn ^= true;
                updateStatus(wnd);
                break;
            case IDM_GRID_DOUBLE:
                g_state.gridSize *= 2;
                updateStatus(wnd);
                break;
            case IDM_GRID_HALF:
                g_state.gridSize /= 2;
                updateStatus(wnd);
                break;
            case IDM_FLY_CAM:
                g_flyCam ^= true;
                updateProjMat();
                break;
            case IDM_DEBUG_INFO:
                for (auto &edge : g_state.surf.edges) {
                    LOG("Edge %08X twin %08X prev %08X next %08X vert %08X face %08X", name(edge),
                        name(edge.second.twin), name(edge.second.prev), name(edge.second.next),
                        name(edge.second.vert), name(edge.second.face));
                }
                LOG("Selected: %08X", name(g_state.sel));
                break;
            /* undoable operations */
            case IDM_JOIN: {
                if (g_state.selVert.find(g_state.surf)) {
                    expectHoverVert();
                    edge_id e1 = edgeOnHoverFace(g_state.surf, g_state.selVert).first;
                    edge_id e2 = edgeOnHoverFace(g_state.surf, g_hover.vert).first;
                    EditorState newState = g_state;
                    newState.surf = mergeVerts(g_state.surf, e1, e2);
                    pushUndo(newState);
                    flashSel(wnd);
                } else if (g_state.selEdge.find(g_state.surf)) {
                    expectHoverVert();
                    edge_id e2 = edgeOnHoverFace(g_state.surf, g_hover.vert).first;
                    EditorState newState = g_state;
                    newState.surf = joinEdgeLoops(g_state.surf, g_state.selEdge, e2);
                    pushUndo(newState);
                    flashSel(wnd);
                } else {
                    throw winged_error(L"No selected vertex or edge");
                }
                break;
            }
            case IDM_MERGE_FACES: {
                expectHoverEdge();
                EditorState newState = g_state;
                newState.surf = mergeFaces(g_state.surf, g_hover.edge);
                pushUndo(newState);
                break;
            }
            case IDM_EXTRUDE: {
                expectSelFace();
                EditorState newState = g_state;
                newState.surf = extrudeFace(g_state.surf, g_state.selFace);
                pushUndo(newState);
                flashSel(wnd);
                break;
            }
            case IDM_FLIP_NORMALS: {
                EditorState newState = g_state;
                newState.surf = flipNormals(g_state.surf);
                pushUndo(newState);
                break;
            }
        }
    } catch (winged_error err) {
        showError(wnd, err);
    }
    refresh(wnd);
}

static void onInitMenu(HWND, HMENU menu) {
    EnableMenuItem(menu, IDM_UNDO, g_undoStack.empty() ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(menu, IDM_REDO, g_redoStack.empty() ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(menu, IDM_CLEAR_SELECT, anySelection() ? MF_ENABLED : MF_GRAYED);
    CheckMenuItem(menu, IDM_TOGGLE_GRID, g_state.gridOn ? MF_CHECKED : MF_UNCHECKED);
    EnableMenuItem(menu, IDM_JOIN, (anySelection() && anyHover()) ? MF_ENABLED : MF_DISABLED);
    EnableMenuItem(menu, IDM_MERGE_FACES,
        g_hover.edge.find(g_state.surf) ? MF_ENABLED : MF_DISABLED);
    EnableMenuItem(menu, IDM_EXTRUDE, g_state.selFace.find(g_state.surf) ? MF_ENABLED : MF_GRAYED);
    CheckMenuItem(menu, IDM_FLY_CAM, g_flyCam ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuRadioItem(menu, toolCommands[0], toolCommands[_countof(toolCommands) - 1],
        toolCommands[g_tool], MF_BYCOMMAND);
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
    auto selEdge = state.selEdge.find(state.surf);

    glLineWidth(1);
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    for (auto &pair : state.surf.edges) {
        if (isPrimary(pair))
            drawEdge(state.surf, pair.second);
    }
    glEnd();
    if (selEdge) {
        glLineWidth(5);
        glColor3f(1, 1, 0);
        glBegin(GL_LINES);
        drawEdge(state.surf, *selEdge);
        glEnd();
    }
    if (auto hoverEdge = g_hover.edge.find(g_state.surf)) {
        glLineWidth(3);
        glColor3f(1, 0.3f, 0.5f);
        glBegin(GL_LINES);
        drawEdge(state.surf, *hoverEdge);
        glEnd();
    }

    glPointSize(7);
    glBegin(GL_POINTS);
    for (auto &pair : state.surf.verts) {
        if (pair.first == state.selVert || (selEdge && pair.first == selEdge->vert)) {
            if (g_flashSel)
                glColor3f(1, 0.8f, 0.8f);
            else
                glColor3f(1, 0, 0);
        } else
            glColor3f(0, 1, 0);
        glVertex3fv(glm::value_ptr(pair.second.pos));
    }
    if (g_tool == TOOL_KNIFE && g_hover.type && g_hover.type != Surface::VERT) {
        glColor3f(1, 1, 1);
        glVertex3fv(glm::value_ptr(g_hover.point));
    }
    glEnd();
    if (auto hoverVert = g_hover.vert.find(g_state.surf)) {
        glPointSize(11);
        glColor3f(1, 1, 1);
        glBegin(GL_POINTS);
        glVertex3fv(glm::value_ptr(hoverVert->pos));
        glEnd();
    }
    
    if (g_tool == TOOL_KNIFE && g_hover.type) {
        if (auto kVert = g_state.selVert.find(state.surf)) {
            glColor3f(1, 1, 1);
            glLineWidth(1);
            glBegin(GL_LINES);
            glVertex3fv(glm::value_ptr(kVert->pos));
            glVertex3fv(glm::value_ptr(g_hover.point));
            glEnd();
        }
    }

    for (auto &pair : state.surf.faces) {
        if (pair.first == state.selFace) {
            if (g_flashSel)
                glColor3f(0, 1, 0.5);
            else
                glColor3f(0, 0.5, 1);
        } else if (g_hover.type && pair.first == g_hoverFace) {
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
    if (!g_flyCam)
        g_mvMat = glm::translate(g_mvMat, glm::vec3(0, 0, -g_zoom));
    g_mvMat = glm::rotate(g_mvMat, g_rotX, glm::vec3(1, 0, 0));
    g_mvMat = glm::rotate(g_mvMat, g_rotY, glm::vec3(0, 1, 0));
    g_mvMat = glm::translate(g_mvMat, g_camPivot);
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
    RegisterClassEx(&wndClass);
    HWND wnd = createWindow(APP_NAME, APP_NAME,
        defaultWindowRect(640, 480, WS_OVERLAPPEDWINDOW, true));
    if (!wnd) return -1;
    ShowWindow(wnd, showCmd);
    return simpleMessageLoop(wnd, LoadAccelerators(instance, L"Accel"));
}

CHROMA_MAIN
