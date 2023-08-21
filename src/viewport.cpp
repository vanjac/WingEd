#include "viewport.h"
#include <queue>
#include <glad.h>
#include <glad_wgl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <immer/set_transient.hpp>
#include "main.h"
#include "ops.h"
#include "resource.h"

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
const float NEAR_CLIP = 0.5f, FAR_CLIP = 500.0f;
const int GRID_SIZE = 128;

const GLchar * const UNIFORM_NAMES[] = {
    "uModelViewMatrix", // UNF_MODELVIEW_MATRIX
    "uProjectionMatrix", // UNF_PROJECTION_MATRIX
    "uNormalMatrix", // UNF_NORMAL_MATRIX
};

const HCURSOR knifeCur = LoadCursor(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_KNIFE));
const HCURSOR drawCur = LoadCursor(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_DRAW));

static PIXELFORMATDESCRIPTOR g_formatDesc;
static HMODULE g_libGL;

static void * loadGLProc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    if ((size_t)p <= (size_t)3 || (size_t)p == (size_t)-1) {
        p = CHECKERR(GetProcAddress(g_libGL, name));
    }
    return p;
}

static void initGL() {
    // https://www.khronos.org/opengl/wiki/Creating_an_OpenGL_Context_(WGL)
    // https://www.khronos.org/opengl/wiki/Load_OpenGL_Functions
    g_formatDesc.nSize = sizeof(g_formatDesc);
    g_formatDesc.nVersion = 1;
    g_formatDesc.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    g_formatDesc.iPixelType = PFD_TYPE_RGBA;
    g_formatDesc.cColorBits = 24;
    g_formatDesc.cDepthBits = 32;
    g_formatDesc.iLayerType = PFD_MAIN_PLANE;

    HWND tempWnd = createWindow(SCRATCH_CLASS.lpszClassName);
    HDC dc = GetDC(tempWnd);
    int pixelFormat = ChoosePixelFormat(dc, &g_formatDesc);
    SetPixelFormat(dc, pixelFormat, &g_formatDesc);
    HGLRC dummyCtx = CHECKERR(wglCreateContext(dc));
    if (!dummyCtx)
        throw winged_error(L"Couldn't create OpenGL context");
    CHECKERR(wglMakeCurrent(dc, dummyCtx));
    g_libGL = CHECKERR(LoadLibrary(L"opengl32.dll"));
    if (!g_libGL)
        throw winged_error(L"Couldn't find opengl32.dll");
    if (!CHECKERR(gladLoadGLLoader(loadGLProc)))
        throw winged_error(L"Failed to load OpenGL");
    if (!CHECKERR(gladLoadWGLLoader(loadGLProc, dc)))
        throw winged_error(L"Failed to load WGL extensions\n");
    if (GLVersion.major < 2)
        throw winged_error(L"OpenGL 2.0 not available");
    CHECKERR(wglMakeCurrent(dc, NULL));
    CHECKERR(wglDeleteContext(dummyCtx));
    ReleaseDC(tempWnd, dc);
    DestroyWindow(tempWnd);
}

bool initViewport() {
    WNDCLASSEX viewClass = makeClass(VIEWPORT_CLASS, windowImplProc);
    viewClass.style = CS_HREDRAW | CS_VREDRAW;
    viewClass.hCursor = NULL;
    RegisterClassEx(&viewClass);

    try {
        initGL();
    } catch (winged_error err) {
        MessageBox(nullptr, err.message, APP_NAME, MB_ICONERROR);
        return false;
    }

    initRenderMesh();
    return true;
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

void ViewportWindow::setViewMode(ViewMode mode) {
    view.mode = mode;
    updateProjMat();
    refresh();
    if (mode != VIEW_ORTHO)
        SetWindowText(wnd, APP_NAME);
}

void ViewportWindow::updateProjMat() {
    HDC dc = GetDC(wnd);
    CHECKERR(wglMakeCurrent(dc, context));
    glViewport(0, 0, (GLsizei)viewportDim.x, (GLsizei)viewportDim.y);
    float aspect = viewportDim.x / viewportDim.y;
    if (view.mode == VIEW_ORTHO) {
        projMat = glm::ortho(-aspect, aspect, -1.0f, 1.0f, -FAR_CLIP / 2, FAR_CLIP / 2);
    } else {
        float fov = glm::radians((view.mode == VIEW_FLY) ? 90.0f : 60.0f);
        projMat = glm::perspective(fov, aspect, NEAR_CLIP, FAR_CLIP);
    }

    for (int i = 0; i < PROG_COUNT; i++) {
        glUseProgram(programs[i].id);
        glUniformMatrix4fv(programs[i].uniforms[UNF_PROJECTION_MATRIX], 1, FALSE,
            glm::value_ptr(projMat));
    }
    ReleaseDC(wnd, dc);
}

glm::vec3 ViewportWindow::forwardAxis() {
    glm::vec3 forward = glm::inverse(mvMat)[2];
    int axis = maxAxis(glm::abs(forward));
    glm::vec3 v = {};
    v[axis] = glm::sign(forward[axis]);
    return v;
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
        type &= view.showElem;
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
        g_mainWindow.refreshAll();
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
            g_state.workPlane.norm = forwardAxis();
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
        moved = {};
        snapAccum = 0;
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
                moved += amount;
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
                amount = diff - moved;
                moved = diff;
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

#ifdef CHROMA_DEBUG
void debugGLCallback(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *msg, const void *) {
    wprintf(L"[OpenGL] %S\n", msg);
}
#endif

static GLuint shaderFromResource(GLenum type, WORD id) {
    GLuint shader = glCreateShader(type);
    DWORD sourceSize;
    auto source = (const GLchar *)getResource(MAKEINTRESOURCE(id), RT_RCDATA, NULL, &sourceSize);
    glShaderSource(shader, 1, &source, (GLint *)&sourceSize);
    glCompileShader(shader);
    return shader;
}

static ShaderProgram programFromShaders(GLuint vert, GLuint frag) {
    ShaderProgram prog;
    prog.id = glCreateProgram();
    glAttachShader(prog.id, vert);
    glAttachShader(prog.id, frag);
    glLinkProgram(prog.id);
    glDetachShader(prog.id, vert);
    glDetachShader(prog.id, frag);
    for (GLint i = 0; i < UNF_COUNT; i++) {
        prog.uniforms[i] = glGetUniformLocation(prog.id, UNIFORM_NAMES[i]);
    }
    return prog;
}

BOOL ViewportWindow::onCreate(HWND, LPCREATESTRUCT) {
    HDC dc = GetDC(wnd);
    int pixelFormat = ChoosePixelFormat(dc, &g_formatDesc);
    SetPixelFormat(dc, pixelFormat, &g_formatDesc);
    const int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 2,
        WGL_CONTEXT_MINOR_VERSION_ARB, 0,
#ifdef CHROMA_DEBUG
        WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB,
#endif
        0
    };
    context = CHECKERR(wglCreateContextAttribsARB(dc, NULL, attribs));
    if (!context) return false;
    CHECKERR(wglMakeCurrent(dc, context));

#ifdef CHROMA_DEBUG
    glDebugMessageCallback(debugGLCallback, NULL);
    // disable warnings for deprecated behavior (TODO: re-enable these eventually)
    glDebugMessageControl(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR, GL_DONT_CARE,
        0, nullptr, GL_FALSE);
    glEnable(GL_DEBUG_OUTPUT);
#endif

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

    glEnableClientState(GL_VERTEX_ARRAY);

    GLuint vertUnlit = shaderFromResource(GL_VERTEX_SHADER, IDR_VERT_UNLIT);
    GLuint vertFace = shaderFromResource(GL_VERTEX_SHADER, IDR_VERT_FACE);
    GLuint fragSolid = shaderFromResource(GL_FRAGMENT_SHADER, IDR_FRAG_SOLID);

    programs[PROG_UNLIT] = programFromShaders(vertUnlit, fragSolid);
    programs[PROG_FACE] = programFromShaders(vertFace, fragSolid);

    glDeleteShader(vertUnlit);
    glDeleteShader(vertFace);
    glDeleteShader(fragSolid);

    ReleaseDC(wnd, dc);
    return true;
}

void ViewportWindow::onDestroy(HWND) {
    wglDeleteContext(context);
}

void ViewportWindow::onClose(HWND) {
    if (g_mainWindow.removeViewport(this)) {
        DestroyWindow(wnd);
        delete this;
    }
}

void ViewportWindow::onActivate(HWND, UINT state, HWND, BOOL minimized) {
    if (state && !minimized)
        g_mainWindow.activeViewport = this;
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
                g_mainWindow.refreshAllImmediate();
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
    g_mainWindow.refreshAll();
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
        moved = {};
        g_mainWindow.updateStatus();
        g_mainWindow.refreshAll();
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
                g_mainWindow.refreshAll();
                break;
            case MOUSE_CAM_ROTATE:
                view.rotX += glm::radians((float)delta.cy) * 0.5f;
                view.rotY += glm::radians((float)delta.cx) * 0.5f;
                refresh();
                SetWindowText(wnd, APP_NAME);
                break;
            case MOUSE_CAM_PAN: {
                bool shift = keyFlags & MK_SHIFT;
                glm::vec3 deltaPos;
                if (view.mode == VIEW_FLY) {
                    deltaPos = shift ? glm::vec3(-delta.cx, delta.cy, 0)
                        : glm::vec3(-delta.cx, 0, -delta.cy);
                } else {
                    deltaPos = shift ? glm::vec3(0, 0, -delta.cy) : glm::vec3(delta.cx, -delta.cy, 0);
                }
                glm::mat4 invMV = glm::inverse(mvMat);
                // there's probably a better way to do this
                glm::mat3 normInvMV = {
                    glm::normalize(invMV[0]), glm::normalize(invMV[1]), glm::normalize(invMV[2])};
                view.camPivot += normInvMV * deltaPos * view.zoom / CAM_MOVE_SCALE;
                refresh();
                break;
            }
        }
        if (mouseMode != MOUSE_TOOL || (g_tool == TOOL_SELECT && (keyFlags & MK_CONTROL))) {
            POINT screenPos = clientToScreen(wnd, lastCurPos);
            SetCursorPos(screenPos.x, screenPos.y);
        } else {
            lastCurPos = curPos;
        }
    }
}

void ViewportWindow::onMouseWheel(HWND, int, int, int delta, UINT) {
    view.zoom *= glm::pow(1.001f, (view.mode == VIEW_FLY) ? delta : -delta);
    refresh();
}

bool ViewportWindow::onCommand(HWND, int id, HWND, UINT) {
    switch (id) {
        case IDM_ORBIT:
            setViewMode(VIEW_ORBIT);
            return true;
        case IDM_VIEW_FLY:
            setViewMode((view.mode == VIEW_FLY) ? VIEW_ORBIT : VIEW_FLY);
            return true;
        case IDM_VIEW_ORTHO:
            setViewMode((view.mode == VIEW_ORTHO) ? VIEW_ORBIT : VIEW_ORTHO);
            return true;
        case IDM_WIREFRAME:
            view.showElem ^= PICK_FACE;
            refresh();
            return true;
        // presets
        case IDM_VIEW_TOP:
            view.rotX = glm::half_pi<float>();
            view.rotY = 0;
            view.showElem = PICK_VERT | PICK_EDGE;
            setViewMode(VIEW_ORTHO);
            SetWindowText(wnd, L"Top");
            return true;
        case IDM_VIEW_FRONT:
            view.rotX = 0;
            view.rotY = 0;
            view.showElem = PICK_VERT | PICK_EDGE;
            setViewMode(VIEW_ORTHO);
            SetWindowText(wnd, L"Front");
            return true;
        case IDM_VIEW_SIDE:
            view.rotX = 0;
            view.rotY = -glm::half_pi<float>();
            view.showElem = PICK_VERT | PICK_EDGE;
            setViewMode(VIEW_ORTHO);
            SetWindowText(wnd, L"Side");
            return true;
        case IDM_PERSPECTIVE:
            view.rotX = glm::radians(30.0f);
            view.rotY = glm::radians(-45.0f);
            view.showElem = PICK_ELEMENT;
            setViewMode(VIEW_ORBIT);
            return true;
        case IDM_FOCUS:
            view.camPivot = -vertsCenter(g_state.surf, selAttachedVerts(g_state));
            refresh();
            return true;
    }
    return false;
}

void ViewportWindow::onSize(HWND, UINT, int cx, int cy) {
    if (cx > 0 && cy > 0) {
        viewportDim = {cx, cy};
        updateProjMat();
    }
}

static void glColorHex(uint32_t color) {
    glColor4ub((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, (color >> 24) & 0xFF);
}

static void drawElementVector(GLenum mode, const std::vector<GLushort> &v) {
    glDrawElements(mode, (GLsizei)v.size(), GL_UNSIGNED_SHORT, v.data());
}

void ViewportWindow::onPaint(HWND) {
    PAINTSTRUCT ps;
    BeginPaint(wnd, &ps);
    CHECKERR(wglMakeCurrent(ps.hdc, context));

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    mvMat = glm::mat4(1);
    if (view.mode == VIEW_ORBIT)
        mvMat = glm::translate(mvMat, glm::vec3(0, 0, -view.zoom));
    else if (view.mode == VIEW_ORTHO)
        mvMat = glm::scale(mvMat, glm::vec3(1 / view.zoom, 1 / view.zoom, 1));
    mvMat = glm::rotate(mvMat, view.rotX, glm::vec3(1, 0, 0));
    mvMat = glm::rotate(mvMat, view.rotY, glm::vec3(0, 1, 0));
    mvMat = glm::translate(mvMat, view.camPivot);
    glm::mat3 normalMat = glm::transpose(glm::inverse(mvMat));

    for (int i = 0; i < PROG_COUNT; i++) {
        glUseProgram(programs[i].id);
        glUniformMatrix4fv(programs[i].uniforms[UNF_MODELVIEW_MATRIX], 1, FALSE,
            glm::value_ptr(mvMat));
        glUniformMatrix3fv(programs[i].uniforms[UNF_NORMAL_MATRIX], 1, FALSE,
            glm::value_ptr(normalMat));
    }

    glUseProgram(programs[PROG_UNLIT].id);

    // axes
    const glm::vec3 axisPoints[] = {{0, 0, 0}, {8, 0, 0}, {0, 8, 0}, {0, 0, 8}};
    const GLubyte xAxisI[] = {0, 1}, yAxisI[] = {0, 2}, zAxisI[] = {0, 3};
    glVertexPointer(3, GL_FLOAT, 0, axisPoints);
    glLineWidth(WIDTH_AXIS);
    glColorHex(COLOR_X_AXIS);
    glDrawElements(GL_LINES, 2, GL_UNSIGNED_BYTE, xAxisI);
    glColorHex(COLOR_Y_AXIS);
    glDrawElements(GL_LINES, 2, GL_UNSIGNED_BYTE, yAxisI);
    glColorHex(COLOR_Z_AXIS);
    glDrawElements(GL_LINES, 2, GL_UNSIGNED_BYTE, zAxisI);

    if (g_renderMeshDirty) {
        generateRenderMesh(&g_renderMesh, g_state);
        g_renderMeshDirty = false;
    }
    drawMesh(g_renderMesh);

    // work plane grid
    bool workPlaneActive = ((TOOL_FLAGS[g_tool] & TOOLF_DRAW)
        && ((g_state.gridOn && g_hover.type) || !(TOOL_FLAGS[g_tool] & TOOLF_HOVFACE)))
        || (g_tool == TOOL_SELECT && mouseMode == MOUSE_TOOL);
    if (workPlaneActive || (view.mode == VIEW_ORTHO && g_state.gridOn)) {
        auto p = g_state.workPlane;
        if (!workPlaneActive) {
            p.norm = forwardAxis();
            glDisable(GL_DEPTH_TEST);
        }
        int axis = maxAxis(glm::abs(p.norm));
        int u = (axis + 1) % 3, v = (axis + 2) % 3;
        glm::vec3 uVec = {}, vVec = {};
        uVec[u] = g_state.gridSize; vVec[v] = g_state.gridSize;
        uVec[axis] = solvePlane(uVec, p.norm, axis);
        vVec[axis] = solvePlane(vVec, p.norm, axis);
        // snap origin to grid
        p.org -= uVec * glm::fract(p.org[u] / g_state.gridSize)
               + vVec * glm::fract(p.org[v] / g_state.gridSize);
        glm::vec3 gridPoints[(GRID_SIZE * 2 + 1) * 4];
        for (int i = -GRID_SIZE, j = 0; i <= GRID_SIZE; i++) {
            gridPoints[j++] = p.org - vVec * (float)GRID_SIZE + uVec * (float)i;
            gridPoints[j++] = p.org + vVec * (float)GRID_SIZE + uVec * (float)i;
            gridPoints[j++] = p.org - uVec * (float)GRID_SIZE + vVec * (float)i;
            gridPoints[j++] = p.org + uVec * (float)GRID_SIZE + vVec * (float)i;
        }
        glVertexPointer(3, GL_FLOAT, 0, gridPoints);
        glEnable(GL_BLEND);
        glEnable(GL_LINE_SMOOTH);
        glLineWidth(WIDTH_GRID);
        glColorHex(COLOR_GRID);
        glDrawArrays(GL_LINES, 0, (GLsizei)_countof(gridPoints));
        glDisable(GL_BLEND);
        glDisable(GL_LINE_SMOOTH);
        if (!workPlaneActive)
            glEnable(GL_DEPTH_TEST);
    }

    SwapBuffers(ps.hdc);
    EndPaint(wnd, &ps);
}

void ViewportWindow::drawMesh(const RenderMesh &mesh) {
    glVertexPointer(3, GL_FLOAT, 0, mesh.vertices.data());
    glNormalPointer(GL_FLOAT, 0, mesh.normals.data());

    if (view.showElem & PICK_EDGE) {
        glLineWidth(WIDTH_EDGE_SEL);
        glColorHex(g_flashSel ? COLOR_EDGE_FLASH : COLOR_EDGE_SEL);
        drawElementVector(GL_LINES, mesh.selEdgeIs);

        glLineWidth(WIDTH_EDGE_HOVER);
        glColorHex(COLOR_EDGE_HOVER);
        drawElementVector(GL_LINES, mesh.hovEdgeIs);
    }

    if (view.showElem & PICK_VERT) {
        glPointSize(SIZE_VERT);
        glColorHex(COLOR_VERT);
        drawElementVector(GL_POINTS, mesh.regVertIs);

        glColorHex(g_flashSel ? COLOR_VERT_FLASH : COLOR_VERT_SEL);
        drawElementVector(GL_POINTS, mesh.selVertIs);

        glColorHex(COLOR_DRAW_POINT);
        drawElementVector(GL_POINTS, mesh.drawPointIs);

        glPointSize(SIZE_VERT_HOVER);
        glColorHex(COLOR_VERT_HOVER);
        drawElementVector(GL_POINTS, mesh.hovVertIs);

        glLineWidth(WIDTH_DRAW);
        glColorHex(COLOR_DRAW_LINE);
        drawElementVector(GL_LINE_STRIP, mesh.drawLineIs);
    }

    if (view.showElem & PICK_EDGE) {
        glLineWidth(WIDTH_EDGE);
        glColorHex(COLOR_EDGE);
        drawElementVector(GL_LINES, mesh.regEdgeIs);
    }

    if (view.showElem & PICK_FACE) {
        glEnableClientState(GL_NORMAL_ARRAY);

        glColorHex(g_flashSel ? COLOR_FACE_FLASH : COLOR_FACE_SEL);
        drawElementVector(GL_TRIANGLES, mesh.selFaceIs);
        glColorHex(COLOR_FACE_HOVER);
        drawElementVector(GL_TRIANGLES, mesh.hovFaceIs);
        glColorHex(COLOR_FACE_ERROR);
        drawElementVector(GL_TRIANGLES, mesh.errFaceIs);

        glColorHex(COLOR_FACE);
        if (view.mode != VIEW_ORTHO)
            glUseProgram(programs[PROG_FACE].id);
        drawElementVector(GL_TRIANGLES, mesh.regFaceIs);
        if (view.mode != VIEW_ORTHO)
            glUseProgram(programs[PROG_UNLIT].id);

        glDisableClientState(GL_NORMAL_ARRAY);
    }
}

LRESULT ViewportWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(wnd, WM_CREATE, onCreate);
        HANDLE_MSG(wnd, WM_DESTROY, onDestroy);
        HANDLE_MSG(wnd, WM_CLOSE, onClose);
        HANDLE_MSG(wnd, WM_ACTIVATE, onActivate);
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
        HANDLE_MSG(wnd, WM_SIZE, onSize);
        HANDLE_MSG(wnd, WM_PAINT, onPaint);
    }
    return DefWindowProc(wnd, msg, wParam, lParam);
}

} // namespace
