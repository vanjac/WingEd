#include "viewport.h"
#include <cfloat>
#include <shlwapi.h>
#include <queue>
#include <glad.h>
#include <glad_wgl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <immer/set_transient.hpp>
#include "main.h"
#include "ops.h"
#include "image.h"
#include "glutil.h"
#include "stdutil.h"
#include "resource.h"
#include "strutil.h"

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
#define COLOR_FACE_HOVER    0xff##ad97ff
#define COLOR_FACE_SEL      0xff##c96bff
#define COLOR_FACE_FLASH    0xff##ff00ff
#define COLOR_FACE_ERROR    0xff##ff0000
#define COLOR_DRAW_POINT    0xff##ffffff
#define COLOR_DRAW_LINE     0xff##ffffff
#define COLOR_GRID          0xaa##575757
#define COLOR_X_AXIS        0xff##ff0000
#define COLOR_Y_AXIS        0xff##00ff00
#define COLOR_Z_AXIS        0xff##0000ff

const auto CAM_MOVE_SCALE = 600.0f;
const auto FOV = 60.0f, NEAR_CLIP = 0.5f, FAR_CLIP = 500.0f;
const auto FLY_FOV = 90.0f, FLY_NEAR_CLIP = 0.2f, FLY_FAR_CLIP = 200.0f;
const auto GRID_SIZE = 128;

enum VertexAttribute {
    ATTR_VERTEX, ATTR_NORMAL, ATTR_COLOR, ATTR_TEXCOORD,
    ATTR_COUNT
};
const GLchar * const ATTRIBUTE_NAMES[] = {
    "aVertex", // ATTR_VERTEX
    "aNormal", // ATTR_NORMAL
    "aColor", // ATTR_COLOR
    "aTexCoord", // ATTR_TEXCOORD
};
const GLchar * const UNIFORM_NAMES[] = {
    "uModelViewMatrix", // UNIF_MODELVIEW_MATRIX
    "uProjectionMatrix", // UNIF_PROJECTION_MATRIX
    "uNormalMatrix", // UNIF_NORMAL_MATRIX
};

const auto knifeCur = LoadCursor(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_KNIFE));
const auto drawCur = LoadCursor(GetModuleHandle(NULL), MAKEINTRESOURCE(IDC_DRAW));

static PIXELFORMATDESCRIPTOR g_formatDesc;
static HMODULE g_libGL;

static void * loadGLProc(const char *name) {
    auto p = void_p(uintptr_t(wglGetProcAddress(name)));
    if (size_t(p) <= size_t(3) || size_t(p) == size_t(-1)) {
        p = void_p(uintptr_t(CHECKERR(GetProcAddress(g_libGL, name))));
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
    g_formatDesc.cColorBits = 32;
    g_formatDesc.cDepthBits = 24;
    g_formatDesc.cStencilBits = 8;
    g_formatDesc.iLayerType = PFD_MAIN_PLANE;

    WNDCLASSEX tempClass = SCRATCH_CLASS;
    tempClass.style |= CS_OWNDC;
    RegisterClassEx(&tempClass);
    auto tempWnd = CHECKERR(createWindow(SCRATCH_CLASS.lpszClassName));
    auto dc = GetDC(tempWnd);
    auto pixelFormat = ChoosePixelFormat(dc, &g_formatDesc);
    SetPixelFormat(dc, pixelFormat, &g_formatDesc);
    auto dummyCtx = CHECKERR(wglCreateContext(dc));
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
    if (!GLAD_GL_VERSION_2_0)
        throw winged_error(L"OpenGL 2.0 not available");
    CHECKERR(wglMakeCurrent(NULL, NULL));
    CHECKERR(wglDeleteContext(dummyCtx));
    ReleaseDC(tempWnd, dc);
    DestroyWindow(tempWnd);
}

bool initViewport() {
    WNDCLASSEX viewClass = makeClass(VIEWPORT_CLASS, windowImplProc);
    viewClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    viewClass.hCursor = NULL;
    RegisterClassEx(&viewClass);

    try {
        initGL();
    } catch (winged_error const& err) {
        MessageBox(nullptr, err.message, APP_NAME, MB_ICONERROR);
        return false;
    }

    initRenderMesh();
    return true;
}

static edge_pair edgeOnHoverFace(const Surface &surf, vert_id v) {
    // TODO: what if there are multiple?
    for (auto edge : VertEdges(surf, v.in(surf))) {
        if (edge.second.face == g_hoverFace)
            return edge;
    }
    throw winged_error();
}

static tuple<edge_id, edge_id> findClosestOpposingEdges(
        const Surface &surf, Face face1, Face face2) {
    float closestDist = FLT_MAX;
    edge_id e1 = face1.edge, e2 = face2.edge;
    for (auto f1Edge : FaceEdges(surf, face1)) {
        auto v1 = f1Edge.second.vert.in(surf).pos;
        for (auto f2Edge : FaceEdges(surf, face2)) {
            auto dist = glm::distance(v1, f2Edge.second.vert.in(surf).pos);
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
            auto erase = toggle && state.selFaces.count(pick.face);
            auto verts = state.selVerts.transient();
            auto faces = state.selFaces.transient();
            auto edges = state.selEdges.transient();
            std::unordered_set<edge_id> visited;
            // flood-fill
            std::queue<edge_id> toSelect;
            toSelect.push(face->edge);
            while (!toSelect.empty()) {
                auto e = toSelect.front();
                toSelect.pop();
                if (!visited.count(e)) {
                    auto edge = e.pair(state.surf);
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
        auto e1 = edgeOnHoverFace(state.surf, *state.selVerts.begin());
        auto e2 = edgeOnHoverFace(state.surf, vert);

        if (e1.first == e2.first) {
            if (g_drawVerts.empty())
                return state; // clicked same vertex twice, nothing to do
            // loop must be clockwise in this case
            auto start = vert.in(state.surf).pos;
            glm::vec3 loopNorm = accumPolyNormal(start, g_drawVerts[0]);
            for (size_t i = 1; i < g_drawVerts.size(); i++)
                loopNorm += accumPolyNormal(g_drawVerts[i - 1], g_drawVerts[i]);
            loopNorm += accumPolyNormal(g_drawVerts.back(), start);

            auto faceNorm = faceNormalNonUnit(state.surf, g_hoverFace.in(state.surf));
            if (glm::dot(loopNorm, faceNorm) > 0)
                std::reverse(g_drawVerts.begin(), g_drawVerts.end());
        }

        edge_id newEdge;
        tie(state.surf, newEdge) = splitFace(std::move(state.surf),
            e1.first, e2.first, g_drawVerts);
        for (size_t i = 0; i < g_drawVerts.size() + 1; i++) {
            auto pair = newEdge.pair(state.surf);
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
    auto faceNorm = faceNormalNonUnit(state.surf, g_hoverFace.in(state.surf));
    if (glm::dot(loopNorm, faceNorm) > 0)
        std::reverse(g_drawVerts.begin() + loopI + 1, g_drawVerts.end());

    auto e = edgeOnHoverFace(state.surf, *state.selVerts.begin());
    edge_id newEdge;
    tie(state.surf, newEdge) = splitFace(std::move(state.surf),
        e.first, e.first, g_drawVerts, loopI);
    for (size_t i = 0; i < g_drawVerts.size() + 1; i++) {
        auto pair = newEdge.pair(state.surf);
        state.selEdges = std::move(state.selEdges).insert(primaryEdge(pair));
        if (int(i) == loopI + 1)
            state.selVerts = immer::set<vert_id>{}.insert(pair.second.vert);
        newEdge = pair.second.next;
    }
    state.selFaces = {};
    g_drawVerts.clear();
    return state;
}

static EditorState join(EditorState state) {
    if (g_hover.vert.find(state.surf) && state.selVerts.size() == 1) {
        auto e1 = edgeOnHoverFace(state.surf, *state.selVerts.begin()).first;
        auto e2 = edgeOnHoverFace(state.surf, g_hover.vert).first;
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
        const auto &face1 = state.selFaces.begin()->in(state.surf);
        auto edges = findClosestOpposingEdges(state.surf, face1, *face2);
        state.surf = joinEdgeLoops(std::move(state.surf), get<0>(edges), get<1>(edges));
        // TODO: select edge loop
    } else {
        throw winged_error();
    }
    return state;
}

void ViewportWindow::invalidateRenderMesh() {
    renderMeshDirtyLocal = true;
}

void ViewportWindow::refresh() {
    InvalidateRect(wnd, NULL, false);
}

void ViewportWindow::refreshImmediate() {
    RedrawWindow(wnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

void ViewportWindow::clearTextureCache() {
    for (const auto &pair : loadedTextures)
        glDeleteTextures(1, &pair.second);
    loadedTextures.clear();
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
    auto dc = GetDC(wnd);
    CHECKERR(wglMakeCurrent(dc, context));
    glViewport(0, 0, GLsizei(viewportDim.x), GLsizei(viewportDim.y));
    auto aspect = viewportDim.x / viewportDim.y;
    if (view.mode == VIEW_ORTHO) {
        projMat = glm::ortho(-aspect, aspect, -1.0f, 1.0f, -FAR_CLIP / 2, FAR_CLIP / 2);
    } else if (view.mode == VIEW_FLY) {
        projMat = glm::perspective(glm::radians(FLY_FOV), aspect, FLY_NEAR_CLIP, FLY_FAR_CLIP);
    } else {
        projMat = glm::perspective(glm::radians(FOV), aspect, NEAR_CLIP, FAR_CLIP);
    }

    for (int i = 0; i < PROG_COUNT; i++) {
        glUseProgram(programs[i].id);
        glUniformMatrix4fv(programs[i].uniforms[UNIF_PROJECTION_MATRIX], 1, FALSE,
            glm::value_ptr(projMat));
    }
    CHECKERR(wglMakeCurrent(NULL, NULL));
    ReleaseDC(wnd, dc);
}

glm::vec3 ViewportWindow::forwardAxis() {
    auto forward = glm::vec3(glm::inverse(mvMat)[2]);
    auto axis = maxAxis(glm::abs(forward));
    glm::vec3 v = {};
    v[axis] = glm::sign(forward[axis]);
    return v;
}

void ViewportWindow::updateHover(POINT pos) {
    auto normCur = screenPosToNDC({pos.x, pos.y}, viewportDim);
    auto project = projMat * mvMat;
    auto grid = g_state.gridOn ? g_state.gridSize : 0;
    PickResult result = {};

    if (TOOL_FLAGS[g_tool] & TOOLF_DRAW) {
        for (size_t i = 0; i < g_drawVerts.size(); i++) {
            auto pickRes = pickVert(g_drawVerts[i], normCur, viewportDim, project);
            if (get<0>(pickRes) && get<1>(pickRes) < result.depth) {
                result.type = PICK_DRAWVERT;
                result.val = i;
                result.point = g_drawVerts[i];
                result.depth = get<1>(pickRes);
            }
        }
    }
    if (g_tool == TOOL_POLY) {
        if (!result.type) {
            auto ray = viewPosToRay(normCur, project);
            auto res = intersectRayPlane(ray, g_state.workPlane);
            if (get<0>(res)) {
                result.point = snapPlanePoint(get<1>(res), g_state.workPlane, grid);
                result.type = PICK_WORKPLANE;
            }
        }
    } else {
        PickType type;
        if (g_state.selMode != SEL_ELEMENTS || (g_tool == TOOL_KNIFE && GetKeyState(VK_MENU) < 0))
            type = PICK_FACE;
        else
            type = PICK_ELEMENT;
        type &= view.showElem;
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
            for (const auto &vert : selAttachedVerts(g_state)) {
                auto point = vert.in(g_state.surf).pos;
                auto dist = glm::dot(point, g_state.workPlane.norm);
                if (dist > closestDist) {
                    g_state.workPlane.org = point;
                    closestDist = dist;
                }
            }
        }
        auto ray = viewPosToRay(screenPosToNDC({pos.x, pos.y}, viewportDim), projMat * mvMat);
        auto res = intersectRayPlane(ray, g_state.workPlane);
        startPlanePos = get<0>(res) ? get<1>(res) : g_state.workPlane.org;
        moved = {};
        snapAccum = 0;
        lockMouse(pos, MOUSE_TOOL);
        g_mainWindow.pushUndo();
    }
}

void ViewportWindow::toolAdjust(POINT pos, SIZE delta, UINT keyFlags) {
    switch (g_tool) {
        case TOOL_SELECT: {
            auto ray = viewPosToRay(screenPosToNDC({pos.x, pos.y}, viewportDim), projMat * mvMat);
            auto res = intersectRayPlane(ray, g_state.workPlane);
            auto planePos = get<0>(res) ? get<1>(res) : g_state.workPlane.org;
            auto absNorm = glm::abs(g_state.workPlane.norm);
            auto normAxis = maxAxis(absNorm);
            auto ortho = bool(keyFlags & MK_CONTROL);
            glm::vec3 amount;
            if (ortho) {
                float push = float(delta.cy) * view.zoom / CAM_MOVE_SCALE;
                if (g_state.gridOn) {
                    auto snap = g_state.gridSize / absNorm[normAxis];
                    snapAccum += push / snap;
                    auto steps = int(glm::floor(snapAccum));
                    snapAccum -= float(steps);
                    push = float(steps) * snap;
                }
                amount = push * g_state.workPlane.norm;
                moved += amount;
                g_state.workPlane.org += amount;
            } else {
                glm::vec3 diff = planePos - startPlanePos;
                if (keyFlags & MK_SHIFT) {
                    auto a = (normAxis + 1) % 3, b = (normAxis + 2) % 3;
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
                auto verts = selAttachedVerts(g_state);
                g_state.surf = transformVertices(std::move(g_state.surf), verts,
                    glm::translate(glm::mat4(1), amount));
                g_mainWindow.updateStatus();
            }
            break;
        }
        default:
            break;
    }
}

#ifdef CHROMA_DEBUG
void APIENTRY debugGLCallback(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *msg, const void *) {
    wprintf(L"[OpenGL] %S\n", msg);
}
#endif

static GLuint shaderFromResource(GLenum type, WORD id) {
    auto shader = glCreateShader(type);
    DWORD sourceSize;
    auto source = reinterpret_cast<const GLchar *>(
        getResource(MAKEINTRESOURCE(id), RT_RCDATA, NULL, &sourceSize));
    glShaderSource(shader, 1, &source, reinterpret_cast<GLint *>(&sourceSize));
    glCompileShader(shader);
    return shader;
}

static ShaderProgram programFromShaders(GLuint vert, GLuint frag) {
    ShaderProgram prog;
    prog.id = glCreateProgram();
    for (GLuint i = 0; i < ATTR_COUNT; i++) {
        glBindAttribLocation(prog.id, i, ATTRIBUTE_NAMES[i]);
    }
    glAttachShader(prog.id, vert);
    glAttachShader(prog.id, frag);
    glLinkProgram(prog.id);
    glDetachShader(prog.id, vert);
    glDetachShader(prog.id, frag);
    for (GLint i = 0; i < UNIF_COUNT; i++) {
        prog.uniforms[i] = glGetUniformLocation(prog.id, UNIFORM_NAMES[i]);
    }
    return prog;
}

static void initSizedBuffer(SizedBuffer *buf, GLenum target, size_t initialSize, GLenum usage) {
    buf->size = initialSize;
    glBufferData(target, initialSize, NULL, usage);
}

static void writeSizedBuffer(SizedBuffer *buf, GLenum target, size_t dataSize, void *data,
        GLenum usage) {
    if (dataSize > buf->size) {
        while (dataSize > buf->size)
            buf->size *= 2;
        glBufferData(target, buf->size, NULL, usage);
    }
    glBufferSubData(target, 0, dataSize, data);
}

BOOL ViewportWindow::onCreate(HWND, LPCREATESTRUCT) {
    auto dc = GetDC(wnd);
    auto pixelFormat = ChoosePixelFormat(dc, &g_formatDesc);
    SetPixelFormat(dc, pixelFormat, &g_formatDesc);
    const int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 2,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
#ifdef CHROMA_DEBUG
        WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB,
#endif
        0
    };
    if (GLAD_WGL_ARB_create_context &&
            ((GLVersion.major == 3 && GLVersion.major >= 2) || GLVersion.major > 3)) {
        context = CHECKERR(wglCreateContextAttribsARB(dc, NULL, attribs));
    } else {
        context = CHECKERR(wglCreateContext(dc));
    }
    if (!context) return false;
    CHECKERR(wglMakeCurrent(dc, context));

#ifdef CHROMA_DEBUG
    if (GLAD_GL_KHR_debug) {
        glDebugMessageCallback(debugGLCallback, NULL);
        // disable warnings for deprecated behavior (TODO: re-enable these eventually)
        glDebugMessageControl(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR, GL_DONT_CARE,
            0, nullptr, GL_FALSE);
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_OTHER, 0,
            GL_DEBUG_SEVERITY_NOTIFICATION, -1, "OpenGL debugging enabled");
    }
#endif

    // TODO: use multiple VAOs
    if (GLAD_GL_ARB_vertex_array_object) {
        GLuint vao;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
    }

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

    glEnableVertexAttribArray(ATTR_VERTEX);

    // static buffers
    glGenBuffers(1, &axisPoints);
    glBindBuffer(GL_ARRAY_BUFFER, axisPoints);
    const glm::vec3 axisPointsData[] = {
        {0, 0, 0}, {8, 0, 0}, {0, 0, 0}, {0, 8, 0}, {0, 0, 0}, {0, 0, 8}};
    glBufferData(GL_ARRAY_BUFFER, sizeof(axisPointsData), axisPointsData, GL_STATIC_DRAW);

    glGenBuffers(1, &gridPoints);
    glBindBuffer(GL_ARRAY_BUFFER, gridPoints);
    glm::vec3 gridPointsData[(GRID_SIZE * 2 + 1) * 4];
    for (int i = -GRID_SIZE, j = 0; i <= GRID_SIZE; i++) {
        gridPointsData[j++] = glm::vec3(i, -GRID_SIZE, 0);
        gridPointsData[j++] = glm::vec3(i,  GRID_SIZE, 0);
        gridPointsData[j++] = glm::vec3(-GRID_SIZE, i, 0);
        gridPointsData[j++] = glm::vec3( GRID_SIZE, i, 0);
    }
    glBufferData(GL_ARRAY_BUFFER, sizeof(gridPointsData), gridPointsData, GL_STATIC_DRAW);

    // dynamic buffers
    glGenBuffers(1, &verticesBuffer.id);
    glGenBuffers(1, &normalsBuffer.id);
    glGenBuffers(1, &texCoordsBuffer.id);
    glGenBuffers(1, &indicesBuffer.id);
    glBindBuffer(GL_ARRAY_BUFFER, verticesBuffer.id);
    initSizedBuffer(&verticesBuffer, GL_ARRAY_BUFFER, 16 * sizeof(glm::vec3), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, normalsBuffer.id);
    initSizedBuffer(&normalsBuffer, GL_ARRAY_BUFFER, 16 * sizeof(glm::vec3), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, texCoordsBuffer.id);
    initSizedBuffer(&texCoordsBuffer, GL_ARRAY_BUFFER, 16 * sizeof(glm::vec2), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indicesBuffer.id);
    initSizedBuffer(&indicesBuffer, GL_ELEMENT_ARRAY_BUFFER, 64 * sizeof(GLushort),
        GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    auto vertUnlit = shaderFromResource(GL_VERTEX_SHADER, IDR_VERT_UNLIT);
    auto vertFace = shaderFromResource(GL_VERTEX_SHADER, IDR_VERT_FACE);
    auto fragSolid = shaderFromResource(GL_FRAGMENT_SHADER, IDR_FRAG_SOLID);
    auto fragFace = shaderFromResource(GL_FRAGMENT_SHADER, IDR_FRAG_FACE);
    auto fragHole = shaderFromResource(GL_FRAGMENT_SHADER, IDR_FRAG_HOLE);

    programs[PROG_UNLIT] = programFromShaders(vertUnlit, fragSolid);
    programs[PROG_FACE] = programFromShaders(vertFace, fragFace);
    programs[PROG_HOLE] = programFromShaders(vertUnlit, fragHole);

    glDeleteShader(vertUnlit);
    glDeleteShader(vertFace);
    glDeleteShader(fragSolid);
    glDeleteShader(fragFace);
    glDeleteShader(fragHole);

    glGenTextures(1, &defTexture);
    glBindTexture(GL_TEXTURE_2D, defTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    auto defHBmp = LoadImage(GetModuleHandle(NULL),
        MAKEINTRESOURCE(IDB_DEFAULT_TEXTURE), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    BITMAP defBmp;
    GetObject(defHBmp, sizeof(defBmp), void_p(&defBmp));
    texImageMipmaps(GL_TEXTURE_2D, GL_RGBA, defBmp.bmWidth, defBmp.bmHeight,
        GL_BGR, GL_UNSIGNED_BYTE, defBmp.bmBits);
    DeleteObject(defHBmp);
    glBindTexture(GL_TEXTURE_2D, 0);

    CHECKERR(wglMakeCurrent(NULL, NULL));
    ReleaseDC(wnd, dc);

    DragAcceptFiles(wnd, TRUE);
    return true;
}

void ViewportWindow::destroy() {
    auto dc = GetDC(wnd);
    CHECKERR(wglMakeCurrent(dc, context)); // doesn't work in WM_DESTROY

    glDeleteBuffers(1, &axisPoints);
    glDeleteBuffers(1, &gridPoints);
    glDeleteBuffers(1, &verticesBuffer.id);
    glDeleteBuffers(1, &normalsBuffer.id);
    glDeleteBuffers(1, &texCoordsBuffer.id);
    glDeleteBuffers(1, &indicesBuffer.id);

    glDeleteTextures(1, &defTexture);
    for (const auto &pair : loadedTextures)
        glDeleteTextures(1, &pair.second);

    for (int i = 0; i < PROG_COUNT; i++)
        glDeleteProgram(programs[i].id);

    CHECKERR(wglMakeCurrent(NULL, NULL));
    ReleaseDC(wnd, dc);

    DestroyWindow(wnd);
}

void ViewportWindow::onDestroy(HWND) {
    CHECKERR(wglDeleteContext(context));
}

void ViewportWindow::onClose(HWND) {
    destroy();
    g_mainWindow.removeViewport(this); // will delete this!
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
                    auto newVert = g_hover.edge.in(newState.surf).next.in(newState.surf).vert;
                    newState = knifeToVert(std::move(newState), newVert);
                    g_mainWindow.pushUndo(std::move(newState));
                    break;
                }
                case PICK_VERT:
                    g_mainWindow.pushUndo(knifeToVert(g_state, g_hover.vert));
                    break;
                case PICK_DRAWVERT:
                    g_mainWindow.pushUndo(knifeToDrawVert(g_state, int(g_hover.val)));
                    break;
                case PICK_FACE:
                    if (g_state.selVerts.size() != 1)
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
                tie(newState.surf, newFace) = makePolygonPlane(g_state.surf, g_drawVerts);
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
            auto toggle = bool(keyFlags & MK_SHIFT);
            auto alreadySelected = hasSelection(g_state);
            if (!alreadySelected) {
                g_state = select(std::move(g_state), g_hover, toggle);
                g_mainWindow.refreshAllImmediate();
            }
            if (DragDetect(wnd, clientToScreen(wnd, {x, y}))) {
                startToolAdjust({x, y});
                g_hover = {};
            } else if (GetKeyState(VK_MENU) < 0 && !g_state.selFaces.empty()
                    && g_hoverFace.find(g_state.surf)) {
                auto paint = g_state.selFaces.begin()->in(g_state.surf).paint;
                EditorState newState = g_state;
                newState.surf = assignPaint(g_state.surf, {g_hoverFace}, paint);
                g_mainWindow.pushUndo(std::move(newState));
            } else if (alreadySelected) {
                if (!toggle)
                    g_state = clearSelection(std::move(g_state));
                g_state = select(std::move(g_state), g_hover, toggle);
            }
        }
    } catch (winged_error const& err) {
        g_mainWindow.showError(err);
#ifndef CHROMA_DEBUG
    } catch (std::exception const& e) {
        g_mainWindow.showStdException(e);
#endif
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
    g_mainWindow.hoveredViewport = this;
    if (!trackMouse) {
        TrackMouseEvent(tempPtr(TRACKMOUSEEVENT{sizeof(TRACKMOUSEEVENT), TME_LEAVE, wnd}));
        trackMouse = true;
    }
    auto curPos = POINT{x, y};
    if (mouseMode == MOUSE_NONE) {
        updateHover(curPos);
    } else if (curPos != lastCurPos) {
        auto delta = SIZE{curPos.x - lastCurPos.x, curPos.y - lastCurPos.y};
        switch (mouseMode) {
            case MOUSE_NONE:
                break;
            case MOUSE_TOOL:
                toolAdjust(curPos, delta, keyFlags);
                g_mainWindow.refreshAll();
                break;
            case MOUSE_CAM_ROTATE:
                view.rotX += glm::radians(float(delta.cy)) * 0.5f;
                view.rotY += glm::radians(float(delta.cx)) * 0.5f;
                refresh();
                SetWindowText(wnd, APP_NAME);
                break;
            case MOUSE_CAM_PAN: {
                auto shift = bool(keyFlags & MK_SHIFT);
                glm::vec3 deltaPos;
                if (view.mode == VIEW_FLY) {
                    deltaPos = shift ? glm::vec3(-delta.cx, delta.cy, 0)
                        : glm::vec3(-delta.cx, 0, -delta.cy);
                } else {
                    deltaPos = shift ? glm::vec3(0, 0, -delta.cy) : glm::vec3(delta.cx, -delta.cy, 0);
                }
                auto invMV = glm::inverse(mvMat);
                // there's probably a better way to do this
                auto normInvMV = glm::mat3{
                    glm::normalize(invMV[0]), glm::normalize(invMV[1]), glm::normalize(invMV[2])};
                view.camPivot += normInvMV * deltaPos * view.zoom / CAM_MOVE_SCALE;
                refresh();
                break;
            }
        }
        if (mouseMode != MOUSE_TOOL || (g_tool == TOOL_SELECT && (keyFlags & MK_CONTROL))) {
            auto screenPos = clientToScreen(wnd, lastCurPos);
            SetCursorPos(screenPos.x, screenPos.y);
        } else {
            lastCurPos = curPos;
        }
    }
}

void ViewportWindow::onMouseLeave(HWND) {
    trackMouse = false;
    if (g_mainWindow.hoveredViewport == this)
        g_mainWindow.hoveredViewport = NULL;
    if (mouseMode == MOUSE_NONE && g_hover.type != PICK_NONE) {
        g_hover = {};
        g_mainWindow.refreshAll();
        if (TOOL_FLAGS[g_tool] & TOOLF_DRAW)
            g_mainWindow.updateStatus();
    }
}

void ViewportWindow::onMouseWheel(HWND, int, int, int delta, UINT) {
    view.zoom *= glm::pow(1.0015f, float((view.mode == VIEW_FLY) ? delta : -delta));
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

void ViewportWindow::onDropFiles(HWND, HDROP drop) {
    try {
        wchar_t path[MAX_PATH];
        if (DragQueryFile(drop, 0, path, _countof(path))) {
            auto ext = PathFindExtension(path);
            if (ext[0] == 0) { // folder
                g_library.rootPath = narrow(path);
            } else if (lstrcmpi(ext, L".wing") == 0) {
                g_mainWindow.open(path);
            } else { // assume image
                auto texFileStr = narrow(path);
                id_t texId = g_library.pathIds[texFileStr];
                if (texId == id_t{}) {
                    texId = genId();
                    g_library.addFile(texId, texFileStr);
                }
                EditorState newState = g_state;
                newState.surf = assignPaint(g_state.surf, g_state.selFaces, Paint{texId});
                g_mainWindow.pushUndo(std::move(newState));
            }
        }
        DragFinish(drop);
    } catch (winged_error const& err) {
        g_mainWindow.showError(err);
    }
    g_mainWindow.updateStatus();
    g_mainWindow.refreshAll();
}

void ViewportWindow::onSize(HWND, UINT, int cx, int cy) {
    if (cx > 0 && cy > 0) {
        viewportDim = {cx, cy};
        updateProjMat();
    }
}

static constexpr glm::vec4 hexColor(uint32_t color) {
    return glm::vec4(
        float((color >> 16) & 0xFF) / 255.0f,
        float((color >> 8) & 0xFF) / 255.0f,
        float(color & 0xFF) / 255.0f,
        float((color >> 24) & 0xFF) / 255.0f);
}

static void setColor(glm::vec4 color) {
    glVertexAttrib4f(ATTR_COLOR, color.r, color.g, color.b, color.a);
}

void ViewportWindow::onPaint(HWND) {
    if (g_renderMeshDirty) {
        g_renderMeshDirty = false;
#ifndef CHROMA_DEBUG
        try {
#endif
            generateRenderMesh(&g_renderMesh, g_state);
#ifndef CHROMA_DEBUG
        } catch (std::exception const& e) {
            g_renderMesh.clear();
            switch (MessageBoxA(NULL, e.what(), "Rendering Error",
                    MB_ICONERROR | MB_ABORTRETRYIGNORE | MB_TASKMODAL)) {
                case IDABORT:
                    SendMessage(g_mainWindow.wnd, WM_CLOSE, 0, 0);
                    break;
                case IDRETRY:
                    g_mainWindow.undo();
                    g_mainWindow.updateStatus();
                    g_mainWindow.refreshAll();
                    break;
            }
            return;
        }
#endif
    }

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
    auto normalMat = glm::mat3(glm::transpose(glm::inverse(mvMat)));

    for (int i = 0; i < PROG_COUNT; i++) {
        glUseProgram(programs[i].id);
        glUniformMatrix4fv(programs[i].uniforms[UNIF_MODELVIEW_MATRIX], 1, FALSE,
            glm::value_ptr(mvMat));
        glUniformMatrix3fv(programs[i].uniforms[UNIF_NORMAL_MATRIX], 1, FALSE,
            glm::value_ptr(normalMat));
    }

    glUseProgram(programs[PROG_UNLIT].id);

    // axes
    glBindBuffer(GL_ARRAY_BUFFER, axisPoints);
    glVertexAttribPointer(ATTR_VERTEX, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glLineWidth(WIDTH_AXIS);
    setColor(hexColor(COLOR_X_AXIS));
    glDrawArrays(GL_LINES, 0, 2);
    setColor(hexColor(COLOR_Y_AXIS));
    glDrawArrays(GL_LINES, 2, 2);
    setColor(hexColor(COLOR_Z_AXIS));
    glDrawArrays(GL_LINES, 4, 2);

    drawMesh(g_renderMesh);

    // work plane grid
    auto workPlaneActive = ((TOOL_FLAGS[g_tool] & TOOLF_DRAW)
        && ((g_state.gridOn && g_hover.type) || !(TOOL_FLAGS[g_tool] & TOOLF_HOVFACE)))
        || (g_tool == TOOL_SELECT && mouseMode == MOUSE_TOOL);
    if (workPlaneActive || (view.mode == VIEW_ORTHO && g_state.gridOn)) {
        Plane p = g_state.workPlane;
        if (!workPlaneActive) {
            p.norm = forwardAxis();
            glDisable(GL_DEPTH_TEST);
        }
        auto axis = maxAxis(glm::abs(p.norm));
        auto u = (axis + 1) % 3, v = (axis + 2) % 3;
        glm::vec3 uVec = {}, vVec = {};
        uVec[u] = g_state.gridSize; vVec[v] = g_state.gridSize;
        uVec[axis] = solvePlane(uVec, p.norm, axis);
        vVec[axis] = solvePlane(vVec, p.norm, axis);
        // snap origin to grid
        p.org -= uVec * glm::fract(p.org[u] / g_state.gridSize)
               + vVec * glm::fract(p.org[v] / g_state.gridSize);
        glm::mat4 gridMat;
        gridMat[0] = glm::vec4(uVec, 0);
        gridMat[1] = glm::vec4(vVec, 0);
        gridMat[2] = glm::vec4(0);
        gridMat[3] = glm::vec4(p.org, 1);
        gridMat = mvMat * gridMat;
        glBindBuffer(GL_ARRAY_BUFFER, gridPoints);
        glVertexAttribPointer(ATTR_VERTEX, 3, GL_FLOAT, GL_FALSE, 0, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnable(GL_BLEND);
        glEnable(GL_LINE_SMOOTH);
        glLineWidth(WIDTH_GRID);
        setColor(hexColor(COLOR_GRID));
        glUseProgram(programs[PROG_UNLIT].id);
        glUniformMatrix4fv(programs[PROG_UNLIT].uniforms[UNIF_MODELVIEW_MATRIX], 1, FALSE,
            glm::value_ptr(gridMat));
        glDrawArrays(GL_LINES, 0, (GRID_SIZE * 2 + 1) * 4);
        glUseProgram(0);
        glDisable(GL_BLEND);
        glDisable(GL_LINE_SMOOTH);
        if (!workPlaneActive)
            glEnable(GL_DEPTH_TEST);
    }

    SwapBuffers(ps.hdc);
    EndPaint(wnd, &ps);
    CHECKERR(wglMakeCurrent(NULL, NULL));
}

void ViewportWindow::drawMesh(const RenderMesh &mesh) {
    if (renderMeshDirtyLocal) {
        renderMeshDirtyLocal = false;
        glBindBuffer(GL_ARRAY_BUFFER, verticesBuffer.id);
        writeSizedBuffer(&verticesBuffer, GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(glm::vec3),
            void_p(mesh.vertices.data()), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, normalsBuffer.id);
        writeSizedBuffer(&normalsBuffer, GL_ARRAY_BUFFER, mesh.normals.size() * sizeof(glm::vec3),
            void_p(mesh.normals.data()), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, texCoordsBuffer.id);
        writeSizedBuffer(&texCoordsBuffer, GL_ARRAY_BUFFER,
            mesh.texCoords.size() * sizeof(glm::vec2), void_p(mesh.texCoords.data()),
            GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indicesBuffer.id);
        writeSizedBuffer(&indicesBuffer, GL_ELEMENT_ARRAY_BUFFER,
            mesh.indices.size() * sizeof(GLushort), void_p(mesh.indices.data()), GL_DYNAMIC_DRAW);
    }

    glBindBuffer(GL_ARRAY_BUFFER, verticesBuffer.id);
    glVertexAttribPointer(ATTR_VERTEX, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, normalsBuffer.id);
    glVertexAttribPointer(ATTR_NORMAL, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, texCoordsBuffer.id);
    glVertexAttribPointer(ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indicesBuffer.id);

    if (view.showElem & PICK_EDGE) {
        glLineWidth(WIDTH_EDGE_SEL);
        setColor(hexColor(g_flashSel ? COLOR_EDGE_FLASH : COLOR_EDGE_SEL));
        drawIndexRange(mesh.ranges[ELEM_SEL_EDGE], GL_LINES);

        glLineWidth(WIDTH_EDGE_HOVER);
        setColor(hexColor(COLOR_EDGE_HOVER));
        drawIndexRange(mesh.ranges[ELEM_HOV_EDGE], GL_LINES);
    }

    if (view.showElem & PICK_VERT) {
        glPointSize(SIZE_VERT);
        setColor(hexColor(COLOR_VERT));
        drawIndexRange(mesh.ranges[ELEM_REG_VERT], GL_POINTS);

        setColor(hexColor(g_flashSel ? COLOR_VERT_FLASH : COLOR_VERT_SEL));
        drawIndexRange(mesh.ranges[ELEM_SEL_VERT], GL_POINTS);

        setColor(hexColor(COLOR_DRAW_POINT));
        drawIndexRange(mesh.ranges[ELEM_DRAW_POINT], GL_POINTS);

        glPointSize(SIZE_VERT_HOVER);
        setColor(hexColor(COLOR_VERT_HOVER));
        drawIndexRange(mesh.ranges[ELEM_HOV_VERT], GL_POINTS);

        glLineWidth(WIDTH_DRAW);
        setColor(hexColor(COLOR_DRAW_LINE));
        drawIndexRange(mesh.ranges[ELEM_DRAW_LINE], GL_LINE_STRIP);
    }

    if ((view.showElem & PICK_EDGE)
            && (g_state.selMode != SEL_SOLIDS || !(view.showElem & PICK_FACE))) {
        glLineWidth(WIDTH_EDGE);
        setColor(hexColor(COLOR_EDGE));
        drawIndexRange(mesh.ranges[ELEM_REG_EDGE], GL_LINES);
    }

    if (view.showElem & PICK_FACE) {
        glEnableVertexAttribArray(ATTR_NORMAL);
        glEnableVertexAttribArray(ATTR_TEXCOORD);
        glUseProgram(programs[PROG_FACE].id);
        for (const auto &faceMesh : mesh.faceMeshes) {
            // generate color from GUID
            auto mat = faceMesh.material;
            auto isHole = (mat == Paint::HOLE_MATERIAL);
            if (isHole)
                glUseProgram(programs[PROG_HOLE].id);
            else
                bindTexture(mat);
            if (faceMesh.state == RenderFaceMesh::HOV) {
                setColor(hexColor(COLOR_FACE_HOVER));
            } else if (faceMesh.state == RenderFaceMesh::SEL) {
                setColor(hexColor(g_flashSel ? COLOR_FACE_FLASH : COLOR_FACE_SEL));
            } else {
                setColor(glm::vec4(1));
            }
            drawIndexRange(faceMesh.range, GL_TRIANGLES);
            if (isHole)
                glUseProgram(programs[PROG_FACE].id);
        }
        glBindTexture(GL_TEXTURE_2D, defTexture);
        setColor(hexColor(COLOR_FACE_ERROR));
        drawIndexRange(mesh.ranges[ELEM_ERR_FACE], GL_TRIANGLES);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisableVertexAttribArray(ATTR_TEXCOORD);
        glDisableVertexAttribArray(ATTR_NORMAL);
    }
    glUseProgram(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void ViewportWindow::drawIndexRange(const IndexRange &range, GLenum mode) {
    glDrawElements(mode, GLsizei(range.count), GL_UNSIGNED_SHORT,
        void_p(range.start * sizeof(GLushort)));
}

void ViewportWindow::bindTexture(id_t texture) {
    if (texture == id_t{}) {
        glBindTexture(GL_TEXTURE_2D, defTexture);
        return;
    }
    GLuint name = loadedTextures[texture];
    if (name) {
        glBindTexture(GL_TEXTURE_2D, name);
    } else {
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        if (auto path = tryGet(g_library.idPaths, texture)) {
            auto image = loadImage(*path);
            if (image.data) {
                texImageMipmaps(GL_TEXTURE_2D, GL_RGBA, image.width, image.height,
                    GL_BGRA, GL_UNSIGNED_BYTE, image.data.get());
            }
        }
        loadedTextures[texture] = name;
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
        case WM_MOUSELEAVE: onMouseLeave(wnd); return 0;
        HANDLE_MSG(wnd, WM_MOUSEWHEEL, onMouseWheel);
        HANDLE_MSG(wnd, WM_COMMAND, onCommand);
        HANDLE_MSG(wnd, WM_DROPFILES, onDropFiles);
        HANDLE_MSG(wnd, WM_SIZE, onSize);
        HANDLE_MSG(wnd, WM_PAINT, onPaint);
    }
    return DefWindowProc(wnd, msg, wParam, lParam);
}

} // namespace
