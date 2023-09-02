// A viewport renders a view of the model and handles mouse input. The user can create multiple
// viewport windows for the same model, each with its own camera and view settings.

#pragma once
#include "common.h"

#define _INC_SHELLAPI // for windowsx.h
#include "winchroma.h"
#undef _INC_SHELLAPI
#include <shellapi.h>
#include "editor.h"
#include "rendermesh.h"
#include <unordered_map>
#include <glm/vec2.hpp>
#include <glm/mat4x4.hpp>

namespace winged {

bool initViewport();

enum MouseMode {
    MOUSE_NONE = 0, MOUSE_TOOL, MOUSE_CAM_ROTATE, MOUSE_CAM_PAN
};

// OpenGL
using buffer_t = unsigned int;
enum ProgramIndex {
    PROG_UNLIT, PROG_FACE, PROG_HOLE,
    PROG_COUNT
};
enum UniformLocation {
    UNIF_MODELVIEW_MATRIX, UNIF_PROJECTION_MATRIX, UNIF_NORMAL_MATRIX,
    UNIF_COUNT
};
struct ShaderProgram {
    unsigned int id;
    int uniforms[UNIF_COUNT];
};
struct SizedBuffer {
    buffer_t id;
    size_t size;
};

const TCHAR VIEWPORT_CLASS[] = _T("WingEd Viewport");
class ViewportWindow : public chroma::WindowImpl {
    const TCHAR * className() const override { return VIEWPORT_CLASS; }

public:
    ViewState view;
    MouseMode mouseMode = MOUSE_NONE;
    glm::vec3 moved;

    void destroy();
    void invalidateRenderMesh();
    void refresh();
    void refreshImmediate();
    void clearTextureCache();
    void updateProjMat();
    glm::vec3 forwardAxis();
    bool onCommand(HWND, int, HWND, UINT);

private:
    HGLRC context;
    glm::mat4 projMat, mvMat;
    glm::vec2 viewportDim;

    bool trackMouse = false;
    POINT lastCurPos;
    glm::vec3 startPlanePos;
    float snapAccum;

    bool renderMeshDirtyLocal = true;
    ShaderProgram programs[PROG_COUNT];
    buffer_t axisPoints, gridPoints;
    SizedBuffer verticesBuffer, normalsBuffer, texCoordsBuffer;
    SizedBuffer indicesBuffer;
    unsigned int defTexture;
    std::unordered_map<id_t, unsigned int> loadedTextures;

    void lockMouse(POINT clientPos, MouseMode mode);
    void setViewMode(ViewMode mode);
    void updateHover(POINT pos);
    void startToolAdjust(POINT pos);
    void toolAdjust(POINT pos, SIZE delta, UINT keyFlags);
    void drawMesh(const RenderMesh &mesh);
    void drawIndexRange(const IndexRange &range, unsigned int mode);
    void bindTexture(id_t tex);

    BOOL onCreate(HWND, LPCREATESTRUCT);
    void onDestroy(HWND);
    void onClose(HWND);
    void onActivate(HWND, UINT, HWND, BOOL);
    bool onSetCursor(HWND, HWND, UINT, UINT);
    void onLButtonDown(HWND, BOOL, int, int, UINT);
    void onRButtonDown(HWND, BOOL, int, int, UINT);
    void onMButtonDown(HWND, BOOL, int, int, UINT);
    void onButtonUp(HWND, int, int, UINT);
    void onMouseMove(HWND, int, int, UINT);
    void onMouseLeave(HWND);
    void onMouseWheel(HWND, int, int, int, UINT);
    void onDropFiles(HWND, HDROP);
    void onSize(HWND, UINT, int, int);
    void onPaint(HWND);
    LRESULT handleMessage(UINT, WPARAM, LPARAM) override;
};

} // namespace
