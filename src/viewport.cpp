#include "viewport.h"
#include <queue>
#include <unordered_set>
#include <gl/GL.h>
#include <gl/GLU.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <immer/set_transient.hpp>
#include "main.h"
#include "ops.h"

using namespace chroma;

namespace winged {

const GLfloat
    SIZE_VERT           = 7,
    SIZE_VERT_HOVER     = 11,
    WIDTH_EDGE          = 1,
    WIDTH_EDGE_HOVER    = 3,
    WIDTH_EDGE_SEL      = 4,
    WIDTH_DRAW          = 2,
    WIDTH_GRID          = 1,
    WIDTH_AXIS          = 1;

#define COLOR_CLEAR         0xff##262626
#define COLOR_VERT          0xff##90ffed
#define COLOR_VERT_HOVER    0xff##ffffff
#define COLOR_VERT_SEL      0xff##ff4d00
#define COLOR_VERT_FLASH    0xff##00ff00
#define COLOR_EDGE          0xff##ffffff
#define COLOR_EDGE_HOVER    0xff##b0004c
#define COLOR_EDGE_SEL      0xff##ff4c7f
#define COLOR_EDGE_FLASH    0xff##fffa6b
#define COLOR_FACE          0xff##6200cb
#define COLOR_FACE_HOVER    0xff##603be5
#define COLOR_FACE_SEL      0xff##a200ff
#define COLOR_FACE_FLASH    0xff##ff00ff
#define COLOR_FACE_ERROR    0xff##ff0000
#define COLOR_DRAW_POINT    0xff##ffffff
#define COLOR_DRAW_LINE     0xff##ffffff
#define COLOR_GRID          0xaa##575757
#define COLOR_X_AXIS        0xff##ff0000
#define COLOR_Y_AXIS        0xff##00ff00
#define COLOR_Z_AXIS        0xff##0000ff

const float CAM_MOVE_SCALE = 600;

const HCURSOR knifeCur = LoadCursor(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_KNIFE));
const HCURSOR drawCur = LoadCursor(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_DRAW));


static GLUtesselator *g_tess;
static GLenum g_tess_error;


static void CALLBACK tessVertexCallback(Vertex *vertex) {
    glVertex3fv(glm::value_ptr(vertex->pos));
}

static void CALLBACK tessErrorCallback(GLenum error) {
    g_tess_error = error;
}

void initViewport() {
    WNDCLASSEX viewClass = makeClass(VIEWPORT_CLASS, windowImplProc);
    viewClass.style = CS_HREDRAW | CS_VREDRAW;
    viewClass.hCursor = NULL;
    RegisterClassEx(&viewClass);

    g_tess = gluNewTess();
    gluTessCallback(g_tess, GLU_TESS_BEGIN, (GLvoid (CALLBACK*) ())glBegin);
    gluTessCallback(g_tess, GLU_TESS_END, (GLvoid (CALLBACK*) ())glEnd);
    gluTessCallback(g_tess, GLU_TESS_VERTEX, (GLvoid (CALLBACK*) ())tessVertexCallback);
    gluTessCallback(g_tess, GLU_TESS_ERROR, (GLvoid (CALLBACK*) ())tessErrorCallback);
    // gluTessCallback(g_tess, GLU_TESS_COMBINE, (GLvoid (*) ())tessCombineCallback);
    // gluTessCallback(g_tess, GLU_TESS_EDGE_FLAG, (GLvoid (*) ())glEdgeFlag);
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

static PickType hoverType() {
    switch (g_hover.type) {
        case PICK_VERT: return g_hover.vert.find(g_state.surf) ? PICK_VERT : PICK_NONE;
        case PICK_FACE: return g_hover.face.find(g_state.surf) ? PICK_FACE : PICK_NONE;
        case PICK_EDGE: return g_hover.edge.find(g_state.surf) ? PICK_EDGE : PICK_NONE;
        default: return g_hover.type;
    }
}

static EditorState select(EditorState state, const PickResult pick, bool toggle) {
    if (state.selMode == SEL_ELEMENTS) {
        switch (pick.type) {
            case PICK_VERT:
                if (state.surf.verts.count(pick.vert)) {
                    if (toggle && state.selVerts.count(pick.vert))
                        state.selVerts = std::move(state.selVerts).erase(pick.vert);
                    else
                        state.selVerts = std::move(state.selVerts).insert(pick.vert);
                }
                break;
            case PICK_FACE:
                if (auto face = pick.face.find(state.surf)) {
                    if (toggle && state.selFaces.count(pick.face)) {
                        state.selFaces = std::move(state.selFaces).erase(pick.face);
                    } else {
                        state.selFaces = std::move(state.selFaces).insert(pick.face);
                        state.workPlane = facePlane(state.surf, *face);
                    }
                }
                break;
            case PICK_EDGE:
                if (auto edge = pick.edge.find(state.surf)) {
                    edge_id e = primaryEdge({pick.edge, *edge});
                    if (toggle && state.selEdges.count(pick.edge))
                        state.selEdges = std::move(state.selEdges).erase(e);
                    else
                        state.selEdges = std::move(state.selEdges).insert(e);
                }
                break;
        }
    } else if (state.selMode == SEL_SOLIDS) {
        if (auto face = pick.face.find(state.surf)) {
            bool erase = toggle && state.selFaces.count(pick.face);
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
                    if (erase) {
                        if (isPrimary(edge)) edges.erase(e);
                        verts.erase(edge.second.vert);
                        faces.erase(edge.second.face);
                    } else {
                        if (isPrimary(edge)) edges.insert(e);
                        verts.insert(edge.second.vert);
                        faces.insert(edge.second.face);
                    }
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

static EditorState knifeToVert(EditorState state, vert_id vert) {
    if (state.selVerts.size() == 1 && g_hoverFace.find(state.surf)) {
        edge_pair e1 = edgeOnHoverFace(state.surf, *state.selVerts.begin());
        edge_pair e2 = edgeOnHoverFace(state.surf, vert);

        if (e1.first == e2.first) {
            if (g_drawVerts.empty())
                return state; // clicked same vertex twice, nothing to do
            // loop must be clockwise in this case
            glm::vec3 start = vert.in(state.surf).pos;
            glm::vec3 loopNorm = accumPolyNormal(start, g_drawVerts[0]);
            for (size_t i = 1; i < g_drawVerts.size(); i++)
                loopNorm += accumPolyNormal(g_drawVerts[i - 1], g_drawVerts[i]);
            loopNorm += accumPolyNormal(g_drawVerts.back(), start);

            glm::vec3 faceNorm = faceNormalNonUnit(state.surf, g_hoverFace.in(state.surf));
            if (glm::dot(loopNorm, faceNorm) > 0)
                std::reverse(g_drawVerts.begin(), g_drawVerts.end());
        }

        edge_id newEdge;
        state.surf = splitFace(std::move(state.surf), e1.first, e2.first, g_drawVerts, &newEdge);
        for (size_t i = 0; i < g_drawVerts.size() + 1; i++) {
            edge_pair pair = newEdge.pair(state.surf);
            state.selEdges = std::move(state.selEdges).insert(primaryEdge(pair));
            newEdge = pair.second.next;
        }
    }

    state.selVerts = immer::set<vert_id>{}.insert(vert);
    state.selFaces = {};
    g_drawVerts.clear();
    return state;
}

static EditorState knifeToDrawVert(EditorState state, int loopI) {
    if (state.selVerts.size() != 1 || !g_hoverFace.find(state.surf))
        throw winged_error();

    glm::vec3 loopNorm = {};
    for (size_t i = loopI, j = g_drawVerts.size() - 1; i < g_drawVerts.size(); j = i++)
        loopNorm += accumPolyNormal(g_drawVerts[j], g_drawVerts[i]);
    glm::vec3 faceNorm = faceNormalNonUnit(state.surf, g_hoverFace.in(state.surf));
    if (glm::dot(loopNorm, faceNorm) > 0)
        std::reverse(g_drawVerts.begin() + loopI + 1, g_drawVerts.end());

    edge_pair e = edgeOnHoverFace(state.surf, *state.selVerts.begin());
    edge_id newEdge;
    state.surf = splitFace(std::move(state.surf), e.first, e.first, g_drawVerts, &newEdge, loopI);
    for (size_t i = 0; i < g_drawVerts.size() + 1; i++) {
        edge_pair pair = newEdge.pair(state.surf);
        state.selEdges = std::move(state.selEdges).insert(primaryEdge(pair));
        if ((int)i == loopI + 1)
            state.selVerts = immer::set<vert_id>{}.insert(pair.second.vert);
        newEdge = pair.second.next;
    }
    state.selFaces = {};
    g_drawVerts.clear();
    return state;
}

static EditorState join(EditorState state) {
    if (g_hover.vert.find(state.surf) && state.selVerts.size() == 1) {
        edge_id e1 = edgeOnHoverFace(state.surf, *state.selVerts.begin()).first;
        edge_id e2 = edgeOnHoverFace(state.surf, g_hover.vert).first;
        state.surf = joinVerts(std::move(state.surf), e1, e2);
    } else if (auto hovEdge = g_hover.edge.find(state.surf)) {
        if (state.selEdges.size() != 1) throw winged_error();
        edge_pair edge1 = state.selEdges.begin()->pair(state.surf);
        edge_pair twin1 = edge1.second.twin.pair(state.surf);
        edge_pair edge2 = {g_hover.edge, *hovEdge};
        edge_pair twin2 = edge2.second.twin.pair(state.surf);
        if (edge1.second.face == edge2.second.face) {} // do nothing
        else if (edge1.second.face == twin2.second.face) {
            std::swap(edge2, twin2);
        } else if (twin1.second.face == edge2.second.face) {
            std::swap(edge1, twin1);
        } else if (twin1.second.face == twin2.second.face) {
            std::swap(edge1, twin1); std::swap(edge2, twin2);
        }
        state.surf = joinEdges(state.surf, edge1.first, edge2.first);
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

void ViewportWindow::refresh() {
    InvalidateRect(wnd, NULL, false);
}

void ViewportWindow::refreshImmediate() {
    RedrawWindow(wnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

void ViewportWindow::lockMouse(POINT clientPos, MouseMode mode) {
    if (mode != MOUSE_TOOL && (mouseMode == MOUSE_NONE || mouseMode == MOUSE_TOOL))
        ShowCursor(false);
    SetCapture(wnd);
    lastCurPos = clientPos;
    mouseMode = mode;
}

void ViewportWindow::updateProjMat() {
    glMatrixMode(GL_PROJECTION);
    float aspect = viewportDim.x / viewportDim.y;
    projMat = glm::perspective(glm::radians(view.flyCam ? 90.0f : 60.0f), aspect, 0.5f, 500.0f);
    glLoadMatrixf(glm::value_ptr(projMat));
    glMatrixMode(GL_MODELVIEW);
}

void ViewportWindow::updateHover(POINT pos) {
    glm::vec2 normCur = screenPosToNDC({pos.x, pos.y}, viewportDim);
    glm::mat4 project = projMat * mvMat;
    float grid = g_state.gridOn ? g_state.gridSize : 0;
    PickResult result = {};

    if (TOOL_FLAGS[g_tool] & TOOLF_DRAW) {
        for (size_t i = 0; i < g_drawVerts.size(); i++) {
            float depth;
            if (pickVert(g_drawVerts[i], normCur, viewportDim, project, &depth)
                    && depth < result.depth) {
                result.type = PICK_DRAWVERT;
                result.val = i;
                result.point = g_drawVerts[i];
                result.depth = depth;
            }
        }
    }
    if (g_tool == TOOL_POLY) {
        if (!result.type) {
            Ray ray = viewPosToRay(normCur, project);
            glm::vec3 planePoint;
            if (intersectRayPlane(ray, g_state.workPlane, &planePoint)) {
                result.point = snapPlanePoint(planePoint, g_state.workPlane, grid);
                result.type = PICK_WORKPLANE;
            }
        }
    } else {
        PickType type = (g_state.selMode == SEL_ELEMENTS) ? PICK_ELEMENT : PICK_FACE;
        if (g_tool == TOOL_KNIFE && GetKeyState(VK_MENU) < 0)
            type &= ~PICK_VERT;
        result = pickElement(g_state.surf, type, normCur, viewportDim, project,
            (g_tool == TOOL_KNIFE) ? grid : 0, result);
    }
    if (TOOL_FLAGS[g_tool] & TOOLF_DRAW && result.type && result.type != PICK_DRAWVERT) {
        for (size_t i = 0; i < g_drawVerts.size(); i++) {
            if (result.point == g_drawVerts[i]) {
                result.type = PICK_DRAWVERT;
                result.val = i;
            }
        }
    }

    if (result.id != g_hover.id || result.point != g_hover.point || result.type != g_hover.type) {
        g_hover = result;
        if (result.type == PICK_FACE) {
            g_hoverFace = g_hover.face;
            if ((TOOL_FLAGS[g_tool] & TOOLF_DRAW) && (TOOL_FLAGS[g_tool] & TOOLF_HOVFACE))
                g_state.workPlane = facePlane(g_state.surf, g_hoverFace.in(g_state.surf));
        }
        refresh();
        if (TOOL_FLAGS[g_tool] & TOOLF_DRAW)
            g_mainWindow.updateStatus();
    }
}

void ViewportWindow::startToolAdjust(POINT pos) {
    if (g_tool == TOOL_SELECT && hasSelection(g_state)) {
        if ((GetKeyState(VK_MENU) < 0)) {
            if (g_state.selFaces.size() == 1) {
                g_state.workPlane = facePlane(g_state.surf,
                    g_state.selFaces.begin()->in(g_state.surf));
            }
        } else {
            glm::vec3 forward = glm::inverse(mvMat)[2];
            int axis = maxAxis(glm::abs(forward));
            g_state.workPlane.norm = {};
            g_state.workPlane.norm[axis] = glm::sign(forward[axis]);
            float closestDist = -FLT_MAX;
            for (auto &vert : selAttachedVerts(g_state)) {
                glm::vec3 point = vert.in(g_state.surf).pos;
                float dist = glm::dot(point, g_state.workPlane.norm);
                if (dist > closestDist) {
                    g_state.workPlane.org = point;
                    closestDist = dist;
                }
            }
        }
        Ray ray = viewPosToRay(screenPosToNDC({pos.x, pos.y}, viewportDim), projMat * mvMat);
        startPlanePos = g_state.workPlane.org; // fallback
        intersectRayPlane(ray, g_state.workPlane, &startPlanePos);
        g_moved = {};
        lockMouse(pos, MOUSE_TOOL);
        g_mainWindow.pushUndo();
    }
}

void ViewportWindow::toolAdjust(POINT pos, SIZE delta, UINT keyFlags) {
    switch (g_tool) {
        case TOOL_SELECT: {
            Ray ray = viewPosToRay(screenPosToNDC({pos.x, pos.y}, viewportDim), projMat * mvMat);
            glm::vec3 planePos = g_state.workPlane.org;
            intersectRayPlane(ray, g_state.workPlane, &planePos);
            glm::vec3 absNorm = glm::abs(g_state.workPlane.norm);
            int normAxis = maxAxis(absNorm);
            bool ortho = keyFlags & MK_CONTROL;
            glm::vec3 amount;
            if (ortho) {
                float push = (float)delta.cy * view.zoom / CAM_MOVE_SCALE;
                if (g_state.gridOn) {
                    float snap = g_state.gridSize / absNorm[normAxis];
                    snapAccum += push / snap;
                    int steps = (int)glm::floor(snapAccum);
                    snapAccum -= steps;
                    push = steps * snap;
                }
                amount = push * g_state.workPlane.norm;
                g_moved += amount;
                g_state.workPlane.org += amount;
            } else {
                glm::vec3 diff = planePos - startPlanePos;
                if (keyFlags & MK_SHIFT) {
                    int a = (normAxis + 1) % 3, b = (normAxis + 2) % 3;
                    if (abs(diff[a]) < abs(diff[b]))
                        diff[a] = 0;
                    else
                        diff[b] = 0;
                }
                if (g_state.gridOn) {
                    glm::vec3 snapped = glm::round(diff / g_state.gridSize) * g_state.gridSize;
                    snapped[normAxis] = diff[normAxis] +
                        solvePlane(snapped - diff, g_state.workPlane.norm, normAxis);
                    diff = snapped;
                }
                amount = diff - g_moved;
                g_moved = diff;
            }
            if (amount != glm::vec3(0)) {
                g_state.surf = transformVertices(std::move(g_state.surf), selAttachedVerts(g_state),
                    glm::translate(glm::mat4(1), amount));
                g_mainWindow.updateStatus();
            }
            break;
        }
    }
}

BOOL ViewportWindow::onCreate(HWND, LPCREATESTRUCT) {
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

    glClearColor(
        ((COLOR_CLEAR >> 16) & 0xFF) / 255.0f,
        ((COLOR_CLEAR >> 8) & 0xFF) / 255.0f,
        (COLOR_CLEAR & 0xFF) / 255.0f,
        ((COLOR_CLEAR >> 24) & 0xFF) / 255.0f);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0, 1.0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glShadeModel(GL_FLAT);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, glm::value_ptr(glm::vec4(.4, .4, .4, 1)));
    glLightfv(GL_LIGHT0, GL_DIFFUSE, glm::value_ptr(glm::vec4(.6, .6, .6, 1)));
    return true;
}

void ViewportWindow::onDestroy(HWND) {
    gluDeleteTess(g_tess);
    if (HGLRC context = wglGetCurrentContext()) {
        HDC dc = wglGetCurrentDC();
        CHECKERR(wglMakeCurrent(NULL, NULL));
        ReleaseDC(wnd, dc);
        wglDeleteContext(context);
    }
}

bool ViewportWindow::onSetCursor(HWND, HWND cursorWnd, UINT hitTest, UINT msg) {
    if (msg && hitTest == HTCLIENT) {
        if (g_tool == TOOL_POLY && g_hover.type) {
            SetCursor(drawCur);
        } else if (g_tool == TOOL_KNIFE && g_hover.type) {
            SetCursor(knifeCur);
        } else if (g_tool == TOOL_JOIN && hasSelection(g_state) && g_hover.type) {
            SetCursor(LoadCursor(NULL, IDC_CROSS));
        } else {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
        }
        return true;
    }
    return FORWARD_WM_SETCURSOR(wnd, cursorWnd, hitTest, msg, DefWindowProc);
}

void ViewportWindow::onLButtonDown(HWND, BOOL, int x, int y, UINT keyFlags) {
    try {
        if (g_tool == TOOL_KNIFE) {
            switch (hoverType()) {
                case PICK_EDGE: {
                    EditorState newState = g_state;
                    newState.surf = splitEdge(g_state.surf, g_hover.edge, g_hover.point);
                    vert_id newVert = g_hover.edge.in(newState.surf).next.in(newState.surf).vert;
                    newState = knifeToVert(std::move(newState), newVert);
                    g_mainWindow.pushUndo(std::move(newState));
                    break;
                }
                case PICK_VERT:
                    g_mainWindow.pushUndo(knifeToVert(g_state, g_hover.vert));
                    break;
                case PICK_DRAWVERT:
                    g_mainWindow.pushUndo(knifeToDrawVert(g_state, (int)g_hover.val));
                    break;
                case PICK_FACE:
                    if (!g_state.selVerts.size() == 1)
                        throw winged_error(); // TODO
                    g_drawVerts.push_back(g_hover.point);
                    break;
                case PICK_NONE:
                    g_state = clearSelection(std::move(g_state));
                    break;
            }
        } else if (g_tool == TOOL_POLY) {
            if (g_hover.type == PICK_WORKPLANE) {
                g_drawVerts.push_back(g_hover.point);
            } else if (g_hover.type == PICK_DRAWVERT && g_hover.val == 0) {
                EditorState newState = clearSelection(g_state);
                face_id newFace;
                newState.surf = makePolygonPlane(g_state.surf, g_drawVerts, &newFace);
                newState.selFaces = std::move(newState.selFaces).insert(newFace);
                g_mainWindow.pushUndo(std::move(newState));
                g_drawVerts.clear();
                if (!(keyFlags & MK_SHIFT))
                    g_tool = TOOL_SELECT;
            } else {
                throw winged_error();
            }
        } else if (g_tool == TOOL_JOIN && hasSelection(g_state) && g_hover.type) {
            g_mainWindow.pushUndo(join(g_state));
            g_mainWindow.flashSel();
            if (!(keyFlags & MK_SHIFT))
                g_tool = TOOL_SELECT;
        } else {
            bool toggle = keyFlags & MK_SHIFT;
            bool alreadySelected = hasSelection(g_state);
            if (!alreadySelected) {
                g_state = select(std::move(g_state), g_hover, toggle);
                refreshImmediate();
            }
            if (DragDetect(wnd, clientToScreen(wnd, {x, y}))) {
                startToolAdjust({x, y});
                g_hover = {};
            } else if (alreadySelected) {
                if (!toggle)
                    g_state = clearSelection(std::move(g_state));
                g_state = select(std::move(g_state), g_hover, toggle);
            }
        }
    } catch (winged_error err) {
        g_mainWindow.showError(err);
    }
    if (!mouseMode)
        updateHover({x, y});
    g_mainWindow.updateStatus();
    refresh();
}

void ViewportWindow::onRButtonDown(HWND, BOOL, int x, int y, UINT) {
    lockMouse({x, y}, MOUSE_CAM_ROTATE);
    g_mainWindow.updateStatus();
}

void ViewportWindow::onMButtonDown(HWND, BOOL, int x, int y, UINT) {
    lockMouse({x, y}, MOUSE_CAM_PAN);
    g_mainWindow.updateStatus();
}

void ViewportWindow::onButtonUp(HWND, int, int, UINT) {
    if (mouseMode) {
        ReleaseCapture();
        if (mouseMode != MOUSE_TOOL)
            ShowCursor(true);
        mouseMode = MOUSE_NONE;
        g_moved = {};
        g_mainWindow.updateStatus();
        refresh();
    }
}

void ViewportWindow::onMouseMove(HWND, int x, int y, UINT keyFlags) {
    POINT curPos = {x, y};
    if (mouseMode == MOUSE_NONE) {
        updateHover(curPos);
    } else if (curPos != lastCurPos) {
        SIZE delta = {curPos.x - lastCurPos.x, curPos.y - lastCurPos.y};
        switch (mouseMode) {
            case MOUSE_TOOL:
                toolAdjust(curPos, delta, keyFlags);
                break;
            case MOUSE_CAM_ROTATE:
                view.rotX += glm::radians((float)delta.cy) * 0.5f;
                view.rotY += glm::radians((float)delta.cx) * 0.5f;
                break;
            case MOUSE_CAM_PAN: {
                bool shift = keyFlags & MK_SHIFT;
                glm::vec3 deltaPos;
                if (view.flyCam) {
                    deltaPos = shift ? glm::vec3(-delta.cx, delta.cy, 0)
                        : glm::vec3(-delta.cx, 0, -delta.cy);
                } else {
                    deltaPos = shift ? glm::vec3(0, 0, -delta.cy) : glm::vec3(delta.cx, -delta.cy, 0);
                }
                deltaPos = glm::inverse(mvMat) * glm::vec4(deltaPos, 0);
                view.camPivot += deltaPos * view.zoom / CAM_MOVE_SCALE;
            }
        }
        refresh();
        if (mouseMode != MOUSE_TOOL || (g_tool == TOOL_SELECT && (keyFlags & MK_CONTROL))) {
            POINT screenPos = clientToScreen(wnd, lastCurPos);
            SetCursorPos(screenPos.x, screenPos.y);
        } else {
            lastCurPos = curPos;
        }
    }
}

void ViewportWindow::onMouseWheel(HWND, int, int, int delta, UINT) {
    view.zoom *= glm::pow(1.001f, view.flyCam ? delta : -delta);
    refresh();
}

void ViewportWindow::onSize(HWND, UINT, int cx, int cy) {
    if (cx > 0 && cy > 0) {
        viewportDim = {cx, cy};
        glViewport(0, 0, cx, cy);
        updateProjMat();
    }
}

void ViewportWindow::onPaint(HWND) {
    PAINTSTRUCT ps;
    BeginPaint(wnd, &ps);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    mvMat = glm::mat4(1);
    if (!view.flyCam)
        mvMat = glm::translate(mvMat, glm::vec3(0, 0, -view.zoom));
    mvMat = glm::rotate(mvMat, view.rotX, glm::vec3(1, 0, 0));
    mvMat = glm::rotate(mvMat, view.rotY, glm::vec3(0, 1, 0));
    mvMat = glm::translate(mvMat, view.camPivot);
    glLoadMatrixf(glm::value_ptr(mvMat));

    drawState(g_state);

    SwapBuffers(ps.hdc);
    EndPaint(wnd, &ps);
}

static void glColorHex(uint32_t color) {
    glColor4ub((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, (color >> 24) & 0xFF);
}

static void drawFace(const Surface &surf, const Face &face) {
    // TODO cache faces
    // https://www.glprogramming.com/red/chapter11.html
    g_tess_error = 0;
    glm::vec3 normal = faceNormal(surf, face);
    glNormal3fv(glm::value_ptr(normal));
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
        glColorHex(COLOR_FACE_ERROR);
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

void ViewportWindow::drawState(const EditorState &state) {
    glLineWidth(WIDTH_AXIS);
    glBegin(GL_LINES);
    glColorHex(COLOR_X_AXIS);
    glVertex3f(0, 0, 0); glVertex3f(8, 0, 0);
    glColorHex(COLOR_Y_AXIS);
    glVertex3f(0, 0, 0); glVertex3f(0, 8, 0);
    glColorHex(COLOR_Z_AXIS);
    glVertex3f(0, 0, 0); glVertex3f(0, 0, 8);
    glEnd();

    if (g_state.selMode == SEL_ELEMENTS) {
        glLineWidth(WIDTH_EDGE_SEL);
        glColorHex(g_flashSel ? COLOR_EDGE_FLASH : COLOR_EDGE_SEL);
        glBegin(GL_LINES);
        for (auto e : state.selEdges) {
            drawEdge(state.surf, e.in(state.surf));
        }
        glEnd();
        if (auto hoverEdge = g_hover.edge.find(state.surf)) {
            glLineWidth(WIDTH_EDGE_HOVER);
            glColorHex(COLOR_EDGE_HOVER);
            glBegin(GL_LINES);
            drawEdge(state.surf, *hoverEdge);
            glEnd();
        }

        glPointSize(SIZE_VERT);
        glBegin(GL_POINTS);
        for (auto &pair : state.surf.verts) {
            if (state.selVerts.count(pair.first)) {
                glColorHex(g_flashSel ? COLOR_VERT_FLASH : COLOR_VERT_SEL);
            } else {
                glColorHex(COLOR_VERT);
            }
            glVertex3fv(glm::value_ptr(pair.second.pos));
        }
        if (TOOL_FLAGS[g_tool] & TOOLF_DRAW) {
            if (numDrawPoints() > 0) {
                for (size_t i = 0; i < g_drawVerts.size(); i++) {
                    if (g_hover.type == PICK_DRAWVERT && g_hover.val == i)
                        glColorHex(COLOR_VERT);
                    else
                        glColorHex(COLOR_DRAW_POINT);
                    glVertex3fv(glm::value_ptr(g_drawVerts[i]));
                }
            }
            if (g_hover.type && g_hover.type != PICK_VERT && g_hover.type != PICK_DRAWVERT) {
                glColorHex(COLOR_DRAW_POINT);
                glVertex3fv(glm::value_ptr(g_hover.point));
            }
        }
        glEnd();
        if (g_hover.type == PICK_DRAWVERT || g_hover.vert.find(state.surf)) {
            glPointSize(SIZE_VERT_HOVER);
            glColorHex(COLOR_VERT_HOVER);
            glBegin(GL_POINTS);
            glVertex3fv(glm::value_ptr(g_hover.point));
            glEnd();
        }

        if (numDrawPoints() + (g_hover.type ? 1 : 0) >= 2) {
            glLineWidth(WIDTH_DRAW);
            glColorHex(COLOR_DRAW_LINE);
            glBegin(GL_LINE_STRIP);
            if (g_tool == TOOL_KNIFE)
                glVertex3fv(glm::value_ptr(state.selVerts.begin()->in(state.surf).pos));
            for (auto &v : g_drawVerts)
                glVertex3fv(glm::value_ptr(v));
            if (g_hover.type)
                glVertex3fv(glm::value_ptr(g_hover.point));
            glEnd();
        }
    }

    glLineWidth(WIDTH_EDGE);
    glColorHex(COLOR_EDGE);
    glBegin(GL_LINES);
    for (auto &pair : state.surf.edges) {
        if (isPrimary(pair))
            drawEdge(state.surf, pair.second);
    }
    glEnd();

    for (auto &pair : state.surf.faces) {
        if (state.selFaces.count(pair.first)) {
            glColorHex(g_flashSel ? COLOR_FACE_FLASH : COLOR_FACE_SEL);
            glDisable(GL_LIGHTING);
        } else if (g_hover.type && pair.first == g_hoverFace
                && (g_hover.type == PICK_FACE || (TOOL_FLAGS[g_tool] & TOOLF_HOVFACE))) {
            glColorHex(COLOR_FACE_HOVER);
            glDisable(GL_LIGHTING);
        } else {
            glColorHex(COLOR_FACE);
            glEnable(GL_LIGHTING);
        }
        drawFace(state.surf, pair.second);
    }
    glDisable(GL_LIGHTING);

    bool drawGrid = (TOOL_FLAGS[g_tool] & TOOLF_DRAW)
        && ((state.gridOn && g_hover.type) || !(TOOL_FLAGS[g_tool] & TOOLF_HOVFACE));
    bool adjustGrid = g_tool == TOOL_SELECT && mouseMode == MOUSE_TOOL;
    if (drawGrid || adjustGrid) {
        auto p = state.workPlane;
        int axis = maxAxis(glm::abs(p.norm));
        int u = (axis + 1) % 3, v = (axis + 2) % 3;
        glm::vec3 uVec = {}, vVec = {};
        uVec[u] = state.gridSize; vVec[v] = state.gridSize;
        uVec[axis] = solvePlane(uVec, p.norm, axis);
        vVec[axis] = solvePlane(vVec, p.norm, axis);
        // snap origin to grid
        p.org -= uVec * glm::fract(p.org[u] / state.gridSize)
               + vVec * glm::fract(p.org[v] / state.gridSize);
        glEnable(GL_BLEND);
        glEnable(GL_LINE_SMOOTH);
        glLineWidth(WIDTH_GRID);
        glColorHex(COLOR_GRID);
        glBegin(GL_LINES);
        for (int i = -128; i <= 128; i++) {
            glVertex3fv(glm::value_ptr(p.org - vVec * 128.0f + uVec * (float)i));
            glVertex3fv(glm::value_ptr(p.org + vVec * 128.0f + uVec * (float)i));
            glVertex3fv(glm::value_ptr(p.org - uVec * 128.0f + vVec * (float)i));
            glVertex3fv(glm::value_ptr(p.org + uVec * 128.0f + vVec * (float)i));
        }
        glEnd();
        glDisable(GL_BLEND);
        glDisable(GL_LINE_SMOOTH);
    }
}

LRESULT ViewportWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(wnd, WM_CREATE, onCreate);
        HANDLE_MSG(wnd, WM_DESTROY, onDestroy);
        HANDLE_MSG(wnd, WM_SETCURSOR, onSetCursor);
        HANDLE_MSG(wnd, WM_LBUTTONDOWN, onLButtonDown);
        HANDLE_MSG(wnd, WM_RBUTTONDOWN, onRButtonDown);
        HANDLE_MSG(wnd, WM_MBUTTONDOWN, onMButtonDown);
        HANDLE_MSG(wnd, WM_LBUTTONUP, onButtonUp);
        HANDLE_MSG(wnd, WM_RBUTTONUP, onButtonUp);
        HANDLE_MSG(wnd, WM_MBUTTONUP, onButtonUp);
        HANDLE_MSG(wnd, WM_MOUSEMOVE, onMouseMove);
        HANDLE_MSG(wnd, WM_MOUSEWHEEL, onMouseWheel);
        HANDLE_MSG(wnd, WM_SIZE, onSize);
        HANDLE_MSG(wnd, WM_PAINT, onPaint);
    }
    return DefWindowProc(wnd, msg, wParam, lParam);
}

} // namespace
