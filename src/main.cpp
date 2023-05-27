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

namespace winged {

const TCHAR APP_NAME[] = _T("WingEd");
const TCHAR VIEWPORT_CLASS[] = _T("WingEd Viewport");

const HCURSOR knifeCur = LoadCursor(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_KNIFE));
const HCURSOR drawCur = LoadCursor(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_DRAW));

enum Tool {
    TOOL_SELECT, TOOL_POLY, TOOL_KNIFE, TOOL_JOIN, NUM_TOOLS
};
enum ToolFlags {
    TOOLF_ELEMENTS = 1<<SEL_ELEMENTS, // allowed in element select mode
    TOOLF_SOLIDS = 1<<SEL_SOLIDS, // allowed in solid select mode
    TOOLF_ALLSEL = TOOLF_ELEMENTS | TOOLF_SOLIDS, // allowed in all select modes
    TOOLF_DRAW = 0x20, // drawing tool (click to add point)
    TOOLF_HOVFACE = 0x40, // show last hovered face while hovering over other elements
};
DEFINE_ENUM_FLAG_OPERATORS(ToolFlags)
struct ToolInfo {
    ToolFlags flags;
    HCURSOR cursor;
    TCHAR *help, *adjustHelp;
};
const ToolInfo tools[] = {
    /*select*/  {TOOLF_ALLSEL, LoadCursor(NULL, IDC_ARROW),
                 L"Click: Select   Shift: Toggle   Drag: Move   Alt-Drag: Move on face plane",
                 L"Shift: Snap axis   Ctrl: Orthogonal"},
    /*poly*/    {TOOLF_ELEMENTS | TOOLF_DRAW, drawCur,
                 L"Click: Add point   Bksp: Delete point   Shift-click: Stay in tool", L""},
    /*knife*/   {TOOLF_ELEMENTS | TOOLF_DRAW | TOOLF_HOVFACE, knifeCur,
                 L"Click: Add vertex   Bksp: Delete vertex   Alt: Ignore vertices", L""},
    /*join*/    {TOOLF_ELEMENTS | TOOLF_HOVFACE, LoadCursor(NULL, IDC_CROSS),
                 L"Click: Select/join   Shift-click: Stay in tool", L""},
};

enum MouseMode {
    MOUSE_NONE = 0, MOUSE_TOOL, MOUSE_CAM_ROTATE, MOUSE_CAM_PAN
};
const TCHAR ROTATE_HELP[] = L"Drag: Orbit";
const TCHAR FLY_ROTATE_HELP[] = L"Drag: Look";
const TCHAR PAN_HELP[] = L"Drag: Pan   Shift: Dolly";
const TCHAR FLY_PAN_HELP[] = L"Drag: Move   Shift: Pan";

enum StatusPart {
    STATUS_SELMODE, STATUS_TOOL, STATUS_GRID, STATUS_SELECT, STATUS_DIMEN, STATUS_HELP,
    NUM_STATUS_PARTS
};

const PickType
    PICK_WORKPLANE = 0x8,
    PICK_DRAWVERT = 0x10;

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


static EditorState g_state;
static std::stack<EditorState> g_undoStack;
static std::stack<EditorState> g_redoStack;
static TCHAR g_fileName[MAX_PATH] = {0};

static Tool g_tool = TOOL_SELECT;
static PickResult g_hover;
static face_id g_hoverFace = {};
static MouseMode g_mouseMode = MOUSE_NONE;
static POINT g_lastCurPos;
static glm::vec3 g_startPlanePos;
static glm::vec3 g_moved;
static float g_snapAccum;
static bool g_flashSel = false;
static std::vector<glm::vec3> g_drawVerts;
static glm::mat4 g_userMatrix = glm::mat4(1);

static ViewState g_view;
static glm::mat4 g_projMat, g_mvMat;
static glm::vec2 g_viewportDim;

static HWND g_mainWnd;
static HWND g_viewportWnd, g_statusWnd;

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

static PickType hoverType() {
    switch (g_hover.type) {
        case PICK_VERT: return g_hover.vert.find(g_state.surf) ? PICK_VERT : PICK_NONE;
        case PICK_FACE: return g_hover.face.find(g_state.surf) ? PICK_FACE : PICK_NONE;
        case PICK_EDGE: return g_hover.edge.find(g_state.surf) ? PICK_EDGE : PICK_NONE;
        default: return g_hover.type;
    }
}

#ifdef CHROMA_DEBUG
static const HEdge expectSingleSelEdge() {
    if (g_state.selEdges.size() == 1)
        return g_state.selEdges.begin()->in(g_state.surf);
    throw winged_error(L"No selected edge");
}
#endif

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

static size_t numDrawPoints() {
    if (g_tool == TOOL_KNIFE) {
        if (g_state.selVerts.size() == 1)
            return g_drawVerts.size() + 1;
        else
            return 0;
    } else if (tools[g_tool].flags & TOOLF_DRAW) {
        return g_drawVerts.size();
    } else {
        return 0;
    }
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

static glm::vec3 vertsCenter(immer::set<vert_id> verts) {
    if (verts.empty())
        return {};
    glm::vec3 min = verts.begin()->in(g_state.surf).pos, max = min;
    for (auto v : verts) {
        auto pos = v.in(g_state.surf).pos;
        min = glm::min(min, pos);
        max = glm::max(max, pos);
    }
    return (min + max) / 2.0f;
}


static void refresh(HWND wnd) {
    InvalidateRect(wnd, NULL, false);
}

static void refreshImmediate(HWND wnd) {
    RedrawWindow(wnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void updateStatus() {
    TCHAR buf[256];

    HMENU menu = GetMenu(g_mainWnd);
    MENUITEMINFO info = {sizeof(info), MIIM_SUBMENU};
    GetMenuItemInfo(menu, IDM_SEL_MENU, false, &info);
    GetMenuString(info.hSubMenu, g_state.selMode, buf, _countof(buf), MF_BYPOSITION);
    if (TCHAR *tab = _tcsrchr(buf, L'\t')) *tab = 0;
    if (TCHAR *amp = _tcsrchr(buf, L'&')) *amp = 6; // ack
    SendMessage(g_statusWnd, SB_SETTEXT, STATUS_SELMODE, (LPARAM)buf);

    GetMenuItemInfo(menu, IDM_TOOL_MENU, false, &info);
    GetMenuString(info.hSubMenu, g_tool, buf, _countof(buf), MF_BYPOSITION);
    if (TCHAR *tab = _tcsrchr(buf, L'\t')) *tab = 0;
    if (TCHAR *amp = _tcsrchr(buf, L'&')) *amp = 6; // ack
    SendMessage(g_statusWnd, SB_SETTEXT, STATUS_TOOL, (LPARAM)buf);

    if (g_state.gridOn)
        _stprintf(buf, L"Grid:  %g", g_state.gridSize);
    else
        _stprintf(buf, L"Grid:  Off");
    SendMessage(g_statusWnd, SB_SETTEXT, STATUS_GRID, (LPARAM)buf);

    buf[0] = 0;
    TCHAR *str = buf;
    if (!g_state.selVerts.empty())
        str += _stprintf(str, L"%zd vert ", g_state.selVerts.size());
    if (!g_state.selEdges.empty())
        str += _stprintf(str, L"%zd edge ", g_state.selEdges.size());
    if (!g_state.selFaces.empty())
        str += _stprintf(str, L"%zd face", g_state.selFaces.size());
    SendMessage(g_statusWnd, SB_SETTEXT, STATUS_SELECT, (LPARAM)buf);

    if (g_mouseMode == MOUSE_TOOL && g_tool == TOOL_SELECT) {
        _stprintf(buf, L"Move  %.3g, %.3g, %.3g", g_moved.x, g_moved.y, g_moved.z);
    } else if (numDrawPoints() > 0 && g_hover.type) {
        glm::vec3 lastPos = g_drawVerts.empty()
            ? g_state.selVerts.begin()->in(g_state.surf).pos
            : g_drawVerts.back();
        _stprintf(buf, L"Len:  %g", glm::distance(lastPos, g_hover.point));
    } else if (g_state.selEdges.size() == 1) {
        HEdge edge = g_state.selEdges.begin()->in(g_state.surf);
        glm::vec3 v1 = edge.vert.in(g_state.surf).pos;
        glm::vec3 v2 = edge.twin.in(g_state.surf).vert.in(g_state.surf).pos;
        _stprintf(buf, L"Len:  %g", glm::distance(v1, v2));
    } else if (g_state.selVerts.size() == 1) {
        glm::vec3 pos = g_state.selVerts.begin()->in(g_state.surf).pos;
        _stprintf(buf, L"Pos:  %.3g, %.3g, %.3g", pos.x, pos.y, pos.z);
    } else {
        buf[0] = 0;
    }
    SendMessage(g_statusWnd, SB_SETTEXT, STATUS_DIMEN, (LPARAM)buf);

    switch (g_mouseMode) {
        case MOUSE_NONE: {
#ifdef CHROMA_DEBUG
            uint32_t selName = 0;
            if (!g_state.selVerts.empty()) selName = name(*g_state.selVerts.begin());
            else if (!g_state.selFaces.empty()) selName = name(*g_state.selFaces.begin());
            else if (!g_state.selEdges.empty()) selName = name(*g_state.selEdges.begin());
            if (selName) {
                _stprintf(buf, L"%08X", selName);
                SendMessage(g_statusWnd, SB_SETTEXT, STATUS_HELP, (LPARAM)buf);
            } else
#endif
            SendMessage(g_statusWnd, SB_SETTEXT, STATUS_HELP, (LPARAM)tools[g_tool].help);
            break;
        }
        case MOUSE_TOOL:
            SendMessage(g_statusWnd, SB_SETTEXT, STATUS_HELP, (LPARAM)tools[g_tool].adjustHelp);
            break;
        case MOUSE_CAM_ROTATE:
            SendMessage(g_statusWnd, SB_SETTEXT, STATUS_HELP,
                (LPARAM)(g_view.flyCam ? FLY_ROTATE_HELP : ROTATE_HELP));
            break;
        case MOUSE_CAM_PAN:
            SendMessage(g_statusWnd, SB_SETTEXT, STATUS_HELP,
                (LPARAM)(g_view.flyCam ? FLY_PAN_HELP : PAN_HELP));
            break;
    }
}

static void showError(winged_error err) {
    if (err.message) {
        MessageBox(g_mainWnd, err.message, APP_NAME, MB_ICONERROR);
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
    if (mode != MOUSE_TOOL && (g_mouseMode == MOUSE_NONE || g_mouseMode == MOUSE_TOOL))
        ShowCursor(false);
    SetCapture(wnd);
    g_lastCurPos = clientPos;
    g_mouseMode = mode;
}

static void setSelMode(SelectMode mode) {
    g_state.selMode = mode;
    if (!(tools[g_tool].flags & (1 << mode)))
        g_tool = TOOL_SELECT;
}

static void resetToolState() {
    g_drawVerts.clear();
}

static void setTool(Tool tool) {
    g_tool = tool;
    resetToolState();
    if (!(tools[tool].flags & (1 << g_state.selMode)))
        g_state.selMode = SEL_ELEMENTS; // TODO
    if ((tools[tool].flags & TOOLF_DRAW) && (tools[tool].flags & TOOLF_HOVFACE))
        if (auto face = g_hoverFace.find(g_state.surf))
            g_state.workPlane = facePlane(g_state.surf, *face);
}


/* VIEWPORT WINDOW */

static void CALLBACK tessVertexCallback(Vertex *vertex) {
    glVertex3fv(glm::value_ptr(vertex->pos));
}

static void CALLBACK tessErrorCallback(GLenum error) {
    g_tess_error = error;
}

static void updateProjMat() {
    glMatrixMode(GL_PROJECTION);
    float aspect = g_viewportDim.x / g_viewportDim.y;
    g_projMat = glm::perspective(glm::radians(g_view.flyCam ? 90.0f : 60.0f), aspect, 0.5f, 500.0f);
    glLoadMatrixf(glm::value_ptr(g_projMat));
    glMatrixMode(GL_MODELVIEW);
}

static BOOL view_onCreate(HWND wnd, LPCREATESTRUCT) {
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

    g_tess = gluNewTess();
    gluTessCallback(g_tess, GLU_TESS_BEGIN, (GLvoid (CALLBACK*) ())glBegin);
    gluTessCallback(g_tess, GLU_TESS_END, (GLvoid (CALLBACK*) ())glEnd);
    gluTessCallback(g_tess, GLU_TESS_VERTEX, (GLvoid (CALLBACK*) ())tessVertexCallback);
    gluTessCallback(g_tess, GLU_TESS_ERROR, (GLvoid (CALLBACK*) ())tessErrorCallback);
    // gluTessCallback(g_tess, GLU_TESS_COMBINE, (GLvoid (*) ())tessCombineCallback);
    // gluTessCallback(g_tess, GLU_TESS_EDGE_FLAG, (GLvoid (*) ())glEdgeFlag);
    return true;
}

static void view_onDestroy(HWND wnd) {
    gluDeleteTess(g_tess);
    if (HGLRC context = wglGetCurrentContext()) {
        HDC dc = wglGetCurrentDC();
        CHECKERR(wglMakeCurrent(NULL, NULL));
        ReleaseDC(wnd, dc);
        wglDeleteContext(context);
    }
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

static bool view_onSetCursor(HWND wnd, HWND cursorWnd, UINT hitTest, UINT msg) {
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

static void startToolAdjust(HWND wnd, POINT pos) {
    if (g_tool == TOOL_SELECT && hasSelection(g_state)) {
        if ((GetKeyState(VK_MENU) < 0)) {
            if (g_state.selFaces.size() == 1) {
                g_state.workPlane = facePlane(g_state.surf,
                    g_state.selFaces.begin()->in(g_state.surf));
            }
        } else {
            glm::vec3 forward = glm::inverse(g_mvMat)[2];
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
        Ray ray = viewPosToRay(screenPosToNDC({pos.x, pos.y}, g_viewportDim), g_projMat * g_mvMat);
        g_startPlanePos = g_state.workPlane.org; // fallback
        intersectRayPlane(ray, g_state.workPlane, &g_startPlanePos);
        g_moved = {};
        lockMouse(wnd, pos, MOUSE_TOOL);
        pushUndo();
    }
}

static void updateHover(HWND wnd, POINT pos) {
    glm::vec2 normCur = screenPosToNDC({pos.x, pos.y}, g_viewportDim);
    glm::mat4 project = g_projMat * g_mvMat;
    float grid = g_state.gridOn ? g_state.gridSize : 0;
    PickResult result = {};

    if (tools[g_tool].flags & TOOLF_DRAW) {
        for (size_t i = 0; i < g_drawVerts.size(); i++) {
            float depth;
            if (pickVert(g_drawVerts[i], normCur, g_viewportDim, project, &depth)
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
        result = pickElement(g_state.surf, type, normCur, g_viewportDim, project,
            (g_tool == TOOL_KNIFE) ? grid : 0, result);
    }
    if (tools[g_tool].flags & TOOLF_DRAW && result.type && result.type != PICK_DRAWVERT) {
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
            if ((tools[g_tool].flags & TOOLF_DRAW) && (tools[g_tool].flags & TOOLF_HOVFACE))
                g_state.workPlane = facePlane(g_state.surf, g_hoverFace.in(g_state.surf));
        }
        refresh(wnd);
        if (tools[g_tool].flags & TOOLF_DRAW)
            updateStatus();
    }
}

static void view_onLButtonDown(HWND wnd, BOOL, int x, int y, UINT keyFlags) {
    try {
        if (g_tool == TOOL_KNIFE) {
            switch (hoverType()) {
                case PICK_EDGE: {
                    EditorState newState = g_state;
                    newState.surf = splitEdge(g_state.surf, g_hover.edge, g_hover.point);
                    vert_id newVert = g_hover.edge.in(newState.surf).next.in(newState.surf).vert;
                    newState = knifeToVert(std::move(newState), newVert);
                    pushUndo(std::move(newState));
                    break;
                }
                case PICK_VERT:
                    pushUndo(knifeToVert(g_state, g_hover.vert));
                    break;
                case PICK_DRAWVERT:
                    pushUndo(knifeToDrawVert(g_state, (int)g_hover.val));
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
                pushUndo(std::move(newState));
                g_drawVerts.clear();
                if (!(keyFlags & MK_SHIFT))
                    g_tool = TOOL_SELECT;
            } else {
                throw winged_error();
            }
        } else if (g_tool == TOOL_JOIN && hasSelection(g_state) && g_hover.type) {
            pushUndo(join(g_state));
            flashSel(wnd);
            if (!(keyFlags & MK_SHIFT))
                g_tool = TOOL_SELECT;
        } else {
            bool toggle = keyFlags & MK_SHIFT;
            bool alreadySelected = hasSelection(g_state);
            if (!alreadySelected) {
                g_state = select(std::move(g_state), g_hover, toggle);
                refreshImmediate(wnd);
            }
            if (DragDetect(wnd, clientToScreen(wnd, {x, y}))) {
                startToolAdjust(wnd, {x, y});
                g_hover = {};
            } else if (alreadySelected) {
                if (!toggle)
                    g_state = clearSelection(std::move(g_state));
                g_state = select(std::move(g_state), g_hover, toggle);
            }
        }
    } catch (winged_error err) {
        showError(err);
    }
    if (!g_mouseMode)
        updateHover(wnd, {x, y});
    updateStatus();
    refresh(wnd);
}

static void view_onRButtonDown(HWND wnd, BOOL, int x, int y, UINT) {
    lockMouse(wnd, {x, y}, MOUSE_CAM_ROTATE);
    updateStatus();
}

static void view_onMButtonDown(HWND wnd, BOOL, int x, int y, UINT) {
    lockMouse(wnd, {x, y}, MOUSE_CAM_PAN);
    updateStatus();
}

static void view_onButtonUp(HWND wnd, int, int, UINT) {
    if (g_mouseMode) {
        ReleaseCapture();
        if (g_mouseMode != MOUSE_TOOL)
            ShowCursor(true);
        g_mouseMode = MOUSE_NONE;
        updateStatus();
        refresh(wnd);
    }
}

static void toolAdjust(POINT pos, SIZE delta, UINT keyFlags) {
    switch (g_tool) {
        case TOOL_SELECT: {
            Ray ray = viewPosToRay(screenPosToNDC({pos.x, pos.y}, g_viewportDim),
                g_projMat * g_mvMat);
            glm::vec3 planePos = g_state.workPlane.org;
            intersectRayPlane(ray, g_state.workPlane, &planePos);
            glm::vec3 absNorm = glm::abs(g_state.workPlane.norm);
            int normAxis = maxAxis(absNorm);
            bool ortho = keyFlags & MK_CONTROL;
            glm::vec3 amount;
            if (ortho) {
                float push = (float)delta.cy * g_view.zoom / CAM_MOVE_SCALE;
                if (g_state.gridOn) {
                    float snap = g_state.gridSize / absNorm[normAxis];
                    g_snapAccum += push / snap;
                    int steps = (int)glm::floor(g_snapAccum);
                    g_snapAccum -= steps;
                    push = steps * snap;
                }
                amount = push * g_state.workPlane.norm;
                g_moved += amount;
                g_state.workPlane.org += amount;
            } else {
                glm::vec3 diff = planePos - g_startPlanePos;
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
                updateStatus();
            }
            break;
        }
    }
}

static void view_onMouseMove(HWND wnd, int x, int y, UINT keyFlags) {
    POINT curPos = {x, y};
    if (g_mouseMode == MOUSE_NONE) {
        updateHover(wnd, curPos);
    } else if (curPos != g_lastCurPos) { // g_mouseMode != MOUSE_NONE
        SIZE delta = {curPos.x - g_lastCurPos.x, curPos.y - g_lastCurPos.y};
        switch (g_mouseMode) {
            case MOUSE_TOOL:
                toolAdjust(curPos, delta, keyFlags);
                break;
            case MOUSE_CAM_ROTATE:
                g_view.rotX += glm::radians((float)delta.cy) * 0.5f;
                g_view.rotY += glm::radians((float)delta.cx) * 0.5f;
                break;
            case MOUSE_CAM_PAN: {
                bool shift = keyFlags & MK_SHIFT;
                glm::vec3 deltaPos;
                if (g_view.flyCam) {
                    deltaPos = shift ? glm::vec3(-delta.cx, delta.cy, 0)
                        : glm::vec3(-delta.cx, 0, -delta.cy);
                } else {
                    deltaPos = shift ? glm::vec3(0, 0, -delta.cy) : glm::vec3(delta.cx, -delta.cy, 0);
                }
                deltaPos = glm::inverse(g_mvMat) * glm::vec4(deltaPos, 0);
                g_view.camPivot += deltaPos * g_view.zoom / CAM_MOVE_SCALE;
            }
        }
        refresh(wnd);
        if (g_mouseMode != MOUSE_TOOL || (g_tool == TOOL_SELECT && (keyFlags & MK_CONTROL))) {
            POINT screenPos = clientToScreen(wnd, g_lastCurPos);
            SetCursorPos(screenPos.x, screenPos.y);
        } else {
            g_lastCurPos = curPos;
        }
    }
}

void view_onMouseWheel(HWND wnd, int, int, int delta, UINT) {
    g_view.zoom *= glm::pow(1.001f, g_view.flyCam ? delta : -delta);
    refresh(wnd);
}

static void view_onSize(HWND, UINT, int cx, int cy) {
    if (cx > 0 && cy > 0) {
        g_viewportDim = {cx, cy};
        glViewport(0, 0, cx, cy);
        updateProjMat();
    }
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

static void drawState(const EditorState &state) {
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
        if (tools[g_tool].flags & TOOLF_DRAW) {
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
                && (g_hover.type == PICK_FACE || (tools[g_tool].flags & TOOLF_HOVFACE))) {
            glColorHex(COLOR_FACE_HOVER);
            glDisable(GL_LIGHTING);
        } else {
            glColorHex(COLOR_FACE);
            glEnable(GL_LIGHTING);
        }
        drawFace(state.surf, pair.second);
    }
    glDisable(GL_LIGHTING);

    bool drawGrid = (tools[g_tool].flags & TOOLF_DRAW)
        && ((g_state.gridOn && g_hover.type) || !(tools[g_tool].flags & TOOLF_HOVFACE));
    bool adjustGrid = g_tool == TOOL_SELECT && g_mouseMode == MOUSE_TOOL;
    if (drawGrid || adjustGrid) {
        auto p = g_state.workPlane;
        int axis = maxAxis(glm::abs(p.norm));
        int u = (axis + 1) % 3, v = (axis + 2) % 3;
        glm::vec3 uVec = {}, vVec = {};
        uVec[u] = g_state.gridSize; vVec[v] = g_state.gridSize;
        uVec[axis] = solvePlane(uVec, p.norm, axis);
        vVec[axis] = solvePlane(vVec, p.norm, axis);
        // snap origin to grid
        p.org -= uVec * glm::fract(p.org[u] / g_state.gridSize)
               + vVec * glm::fract(p.org[v] / g_state.gridSize);
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

static void view_onPaint(HWND wnd) {
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

static LRESULT CALLBACK ViewportWindowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(wnd, WM_CREATE, view_onCreate);
        HANDLE_MSG(wnd, WM_DESTROY, view_onDestroy);
        HANDLE_MSG(wnd, WM_SETCURSOR, view_onSetCursor);
        HANDLE_MSG(wnd, WM_LBUTTONDOWN, view_onLButtonDown);
        HANDLE_MSG(wnd, WM_RBUTTONDOWN, view_onRButtonDown);
        HANDLE_MSG(wnd, WM_MBUTTONDOWN, view_onMButtonDown);
        HANDLE_MSG(wnd, WM_LBUTTONUP, view_onButtonUp);
        HANDLE_MSG(wnd, WM_RBUTTONUP, view_onButtonUp);
        HANDLE_MSG(wnd, WM_MBUTTONUP, view_onButtonUp);
        HANDLE_MSG(wnd, WM_MOUSEMOVE, view_onMouseMove);
        HANDLE_MSG(wnd, WM_MOUSEWHEEL, view_onMouseWheel);
        HANDLE_MSG(wnd, WM_SIZE, view_onSize);
        HANDLE_MSG(wnd, WM_PAINT, view_onPaint);
    }
    return DefWindowProc(wnd, msg, wParam, lParam);
}


/* MAIN WINDOW */

static BOOL main_onCreate(HWND wnd, LPCREATESTRUCT) {
    g_mainWnd = wnd;
    g_viewportWnd = createChildWindow(wnd, VIEWPORT_CLASS);
    g_statusWnd = CreateStatusWindow(
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | CCS_BOTTOM | SBARS_SIZEGRIP,
        NULL, wnd, 0);
    int parts[NUM_STATUS_PARTS];
    int x = 0;
    x += 60; parts[STATUS_SELMODE] = x;
    x += 60; parts[STATUS_TOOL] = x;
    x += 70; parts[STATUS_GRID] = x;
    x += 150; parts[STATUS_SELECT] = x;
    x += 150; parts[STATUS_DIMEN] = x;
    parts[NUM_STATUS_PARTS - 1] = -1;
    SendMessage(g_statusWnd, SB_SETPARTS, NUM_STATUS_PARTS, (LPARAM)parts);
    updateStatus();
    return true;
}

static void main_onNCDestroy(HWND) {
    PostQuitMessage(0);
}

static void main_onSize(HWND, UINT, int cx, int cy) {
    int statusHeight = rectHeight(windowRect(g_statusWnd));
    MoveWindow(g_statusWnd, 0, cy - statusHeight, cx, statusHeight, true);
    MoveWindow(g_viewportWnd, 0, 0, cx, cy - statusHeight, true);
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
                    newState.surf = joinVerts(std::move(newState.surf), edge.prev, vert->edge);
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

INT_PTR CALLBACK matrixDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtr(dlg, DWLP_USER, lParam);
            glm::mat4 &mat = *(glm::mat4 *)lParam;
            for (int i = 0; i < 9; i++) {
                TCHAR buf[64];
                _stprintf(buf, L"%g", mat[i % 3][i / 3]);
                SetDlgItemText(dlg, 1000 + i, buf);
            }
            return true;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK:
                case IDCANCEL: {
                    glm::mat4 &mat = *(glm::mat4 *)GetWindowLongPtr(dlg, DWLP_USER);
                    for (int i = 0; i < 9; i++) {
                        TCHAR buf[64];
                        GetDlgItemText(dlg, 1000 + i, buf, _countof(buf));
                        mat[i % 3][i / 3] = (float)_ttof(buf);
                    }
                    EndDialog(dlg, LOWORD(wParam));
                    return true;
                }
            }
            break;
    }
    return false;
}

static void main_onCommand(HWND wnd, int id, HWND ctl, UINT) {
    if (ctl) return;

    try {
        switch (id) {
            /* File */
            case IDM_NEW:
                if (MessageBox(wnd, L"Are you sure?", L"New File", MB_YESNO) == IDYES) {
                    g_state = {};
                    g_view = {};
                    g_undoStack = {};
                    g_redoStack = {};
                    g_fileName[0] = 0;
                    resetToolState();
                }
                break;
            case IDM_OPEN: {
                TCHAR fileName[MAX_PATH];
                fileName[0] = 0;
                const TCHAR filters[] = L"WingEd File (.wing)\0*.wing\0\0";
                if (GetOpenFileName(tempPtr(makeOpenFileName(fileName, wnd, filters, L"wing")))) {
                    std::tie(g_state, g_view) = readFile(fileName);
                    g_undoStack = {};
                    g_redoStack = {};
                    memcpy(g_fileName, fileName, sizeof(g_fileName));
                    resetToolState();
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
            case IDM_EXPORT_OBJ: {
                TCHAR fileName[MAX_PATH];
                fileName[0] = 0;
                const TCHAR filters[] = L"OBJ file (.obj)\0*.obj\0All Files\0*.*\0\0";
                if (GetSaveFileName(tempPtr(makeOpenFileName(fileName, wnd, filters, L"obj"))))
                    writeObj(fileName, g_state.surf);
                break;
            }
            /* Tool */
            case IDM_TOOL_SELECT:
                setTool(TOOL_SELECT);
                break;
            case IDM_TOOL_POLY:
                setTool(TOOL_POLY);
                if (g_state.selFaces.size() == 1) {
                    g_state.workPlane = facePlane(g_state.surf,
                        g_state.selFaces.begin()->in(g_state.surf));
                }
                break;
            case IDM_TOOL_KNIFE:
                setTool(TOOL_KNIFE);
                break;
            case IDM_TOOL_JOIN:
                setTool(TOOL_JOIN);
                break;
            /* Select */
            case IDM_CLEAR_SELECT:
                g_state = clearSelection(std::move(g_state));
                resetToolState();
                break;
            case IDM_SEL_ELEMENTS:
                setSelMode(SEL_ELEMENTS);
                break;
            case IDM_SEL_SOLIDS:
                if (g_state.selMode != SEL_SOLIDS) {
                    g_state = clearSelection(std::move(g_state));
                    g_hoverFace = {};
                    g_hover.type = PICK_NONE;
                }
                setSelMode(SEL_SOLIDS);
                break;
#ifdef CHROMA_DEBUG
            case IDM_EDGE_TWIN:
                g_state.selEdges = immer::set<edge_id>{}.insert(expectSingleSelEdge().twin);
                break;
            case IDM_NEXT_FACE_EDGE:
                g_state.selEdges = immer::set<edge_id>{}.insert(expectSingleSelEdge().next);
                break;
            case IDM_PREV_FACE_EDGE:
                g_state.selEdges = immer::set<edge_id>{}.insert(expectSingleSelEdge().prev);
                break;
#endif
            /* View */
            case IDM_FLY_CAM:
                g_view.flyCam ^= true;
                updateProjMat();
                break;
            case IDM_FOCUS:
                g_view.camPivot = -vertsCenter(selAttachedVerts(g_state));
                break;
            /* Edit */
            case IDM_UNDO:
                if (!g_undoStack.empty()) {
                    g_redoStack.push(g_state);
                    g_state = g_undoStack.top();
                    g_undoStack.pop();
                }
                resetToolState();
                break;
            case IDM_REDO:
                if (!g_redoStack.empty()) {
                    g_undoStack.push(g_state);
                    g_state = g_redoStack.top();
                    g_redoStack.pop();
                }
                resetToolState();
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
            case IDM_DRAW_BKSP:
                if (!g_drawVerts.empty())
                    g_drawVerts.pop_back();
                break;
            /* undoable operations... */
            case IDM_ERASE:
                pushUndo(erase(g_state));
                break;
            /* element */
            case IDM_EXTRUDE: {
                EditorState newState = g_state;
                newState.selVerts = {};
                newState.selEdges = {};
                for (auto f : g_state.selFaces) {
                    immer::set_transient<edge_id> extEdges;
                    for (auto e : g_state.selEdges) {
                        HEdge edge = e.in(newState.surf);
                        if (edge.face == f)
                            extEdges.insert(e);
                        else if (edge.twin.in(newState.surf).face == f)
                            extEdges.insert(edge.twin);
                    }
                    newState.surf = extrudeFace(newState.surf, f, extEdges.persistent());
                    for (auto e : extEdges) {
                        auto primary = primaryEdge(e.pair(newState.surf));
                        newState.selEdges = std::move(newState.selEdges).insert(primary);
                    }
                }
                pushUndo(std::move(newState));
                flashSel(g_viewportWnd);
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
                flashSel(g_viewportWnd);
                break;
            }
            /* solid */
            case IDM_DUPLICATE: {
                EditorState newState = g_state;
                newState.surf = duplicate(g_state.surf,
                    g_state.selEdges, g_state.selVerts, g_state.selFaces);
                pushUndo(std::move(newState));
                break;
            }
            case IDM_FLIP_NORMALS: {
                EditorState newState = g_state;
                if (g_state.selMode == SEL_SOLIDS && hasSelection(g_state))
                    newState.surf = flipNormals(g_state.surf, g_state.selEdges, g_state.selVerts);
                else
                    newState.surf = flipAllNormals(g_state.surf);
                pushUndo(std::move(newState));
                break;
            }
            case IDM_SNAP: {
                EditorState newState = g_state;
                newState.surf = snapVertices(g_state.surf,
                    selAttachedVerts(g_state), g_state.gridSize);
                pushUndo(std::move(newState));
                break;
            }
            case IDM_TRANSFORM_MATRIX: {
                if (DialogBoxParam(GetModuleHandle(NULL), L"IDD_MATRIX", wnd, matrixDlgProc,
                        (LPARAM)&g_userMatrix) == IDOK) {
                    auto verts = selAttachedVerts(g_state);
                    glm::vec3 center = vertsCenter(verts);
                    EditorState newState = g_state;
                    newState.surf = transformVertices(g_state.surf, verts, glm::translate(
                        glm::translate(glm::mat4(1), center) * g_userMatrix, -center));
                    pushUndo(std::move(newState));
                }
                break;
            }
        }
    } catch (winged_error err) {
        showError(err);
    }
    updateStatus();
    refresh(g_viewportWnd);
}

static void main_onInitMenu(HWND, HMENU menu) {
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
    EnableMenuItem(menu, IDM_DUPLICATE, (hasSel && selSolid) ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_TRANSFORM_MATRIX, hasSel ? MF_ENABLED : MF_GRAYED);
    CheckMenuItem(menu, IDM_FLY_CAM, g_view.flyCam ? MF_CHECKED : MF_UNCHECKED);
    EnableMenuItem(menu, IDM_FOCUS, hasSel ? MF_ENABLED : MF_GRAYED);

    MENUITEMINFO selMenu = {sizeof(selMenu), MIIM_SUBMENU};
    GetMenuItemInfo(menu, IDM_SEL_MENU, false, &selMenu);
    CheckMenuRadioItem(selMenu.hSubMenu, 0, NUM_SELMODES - 1, g_state.selMode, MF_BYPOSITION);

    MENUITEMINFO toolMenu = {sizeof(toolMenu), MIIM_SUBMENU};
    GetMenuItemInfo(menu, IDM_TOOL_MENU, false, &toolMenu);
    CheckMenuRadioItem(toolMenu.hSubMenu, 0, NUM_TOOLS - 1, g_tool, MF_BYPOSITION);
}

static void main_onMenuSelect(HWND, UINT msg, WPARAM wParam, LPARAM lParam) {
    MenuHelp(msg, wParam, lParam, NULL, GetModuleHandle(NULL), g_statusWnd, tempPtr(0u));
}

static LRESULT CALLBACK MainWindowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(wnd, WM_CREATE, main_onCreate);
        HANDLE_MSG(wnd, WM_NCDESTROY, main_onNCDestroy);
        HANDLE_MSG(wnd, WM_SIZE, main_onSize);
        HANDLE_MSG(wnd, WM_COMMAND, main_onCommand);
        HANDLE_MSG(wnd, WM_INITMENU, main_onInitMenu);
        case WM_MENUSELECT: main_onMenuSelect(wnd, msg, wParam, lParam); return 0;
    }
    return DefWindowProc(wnd, msg, wParam, lParam);
}

} // namespace

using namespace winged;

int APIENTRY _tWinMain(HINSTANCE instance, HINSTANCE, LPTSTR, int showCmd) {
    WNDCLASSEX mainClass = makeClass(APP_NAME, MainWindowProc);
    mainClass.lpszMenuName = APP_NAME;
    RegisterClassEx(&mainClass);
    WNDCLASSEX viewClass = makeClass(VIEWPORT_CLASS, ViewportWindowProc);
    viewClass.style = CS_HREDRAW | CS_VREDRAW;
    viewClass.hCursor = NULL;
    RegisterClassEx(&viewClass);
    HWND wnd = createWindow(APP_NAME, APP_NAME,
        defaultWindowRect(640, 480, WS_OVERLAPPEDWINDOW, true));
    if (!wnd) return -1;
    ShowWindow(wnd, showCmd);
    return simpleMessageLoop(wnd, LoadAccelerators(instance, L"Accel"));
}

CHROMA_MAIN
