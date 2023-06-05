// A viewport renders a view of the model and handles mouse input. The user can create multiple
// viewport windows for the same model, each with its own camera and view settings.

#pragma once
#include "common.h"
#include "winchroma.h"
#include "editor.h"
#include <glm/vec2.hpp>
#include <glm/mat4x4.hpp>

namespace winged {

void initViewport();

enum MouseMode {
    MOUSE_NONE = 0, MOUSE_TOOL, MOUSE_CAM_ROTATE, MOUSE_CAM_PAN
};

const TCHAR VIEWPORT_CLASS[] = _T("WingEd Viewport");
class ViewportWindow : public chroma::WindowImpl {
    const TCHAR * className() const override { return VIEWPORT_CLASS; }

public:
    ViewState view;
    MouseMode mouseMode = MOUSE_NONE;
    glm::vec3 moved;

    void refresh();
    void refreshImmediate();
    glm::vec3 forwardAxis();
    bool onCommand(HWND, int, HWND, UINT);

private:
    HGLRC context;
    glm::mat4 projMat, mvMat;
    glm::vec2 viewportDim;

    POINT lastCurPos;
    glm::vec3 startPlanePos;
    float snapAccum;

    void lockMouse(POINT clientPos, MouseMode mode);
    void setViewMode(ViewMode mode);
    void updateProjMat();
    void updateHover(POINT pos);
    void startToolAdjust(POINT pos);
    void toolAdjust(POINT pos, SIZE delta, UINT keyFlags);
    void drawState(const EditorState &state);

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
    void onMouseWheel(HWND, int, int, int, UINT);
    void onSize(HWND, UINT, int, int);
    void onPaint(HWND);
    LRESULT handleMessage(UINT, WPARAM, LPARAM) override;
};

} // namespace
