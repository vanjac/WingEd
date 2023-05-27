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
struct ViewportWindow : chroma::WindowImpl {
    ViewState view;
    glm::mat4 projMat, mvMat;
    glm::vec2 viewportDim;

    MouseMode mouseMode = MOUSE_NONE;
    POINT lastCurPos;
    glm::vec3 startPlanePos;
    float snapAccum;

    HGLRC context;

    const TCHAR * className() const { return VIEWPORT_CLASS; }

    void refresh();
    void refreshImmediate();
    void lockMouse(POINT clientPos, MouseMode mode);
    void updateProjMat();
    void updateHover(POINT pos);
    void startToolAdjust(POINT pos);
    void toolAdjust(POINT pos, SIZE delta, UINT keyFlags);
    void drawState(const EditorState &state);

    BOOL onCreate(HWND, LPCREATESTRUCT);
    void onDestroy(HWND);
    void onClose(HWND);
    bool onSetCursor(HWND, HWND, UINT, UINT);
    void onLButtonDown(HWND, BOOL, int, int, UINT);
    void onRButtonDown(HWND, BOOL, int, int, UINT);
    void onMButtonDown(HWND, BOOL, int, int, UINT);
    void onButtonUp(HWND, int, int, UINT);
    void onMouseMove(HWND, int, int, UINT);
    void onMouseWheel(HWND, int, int, int, UINT);
    bool onCommand(HWND, int, HWND, UINT);
    void onSize(HWND, UINT, int, int);
    void onPaint(HWND);
    LRESULT handleMessage(UINT, WPARAM, LPARAM) override;
};

} // namespace
