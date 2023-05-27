#pragma once
#include "common.h"

#include <vector>
#include <stack>
#include "winchroma.h"
#include "editor.h"
#include "viewport.h"
#include "picking.h"
#include "resource.h"

namespace winged {

const TCHAR APP_NAME[] = _T("WingEd");

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
    HCURSOR cursor; // TODO move cursors to viewport.cpp
    TCHAR *help, *adjustHelp; // TODO move help text to main.cpp
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

struct MainWindow : chroma::WindowImpl {
    std::stack<EditorState> undoStack;
    std::stack<EditorState> redoStack;
    TCHAR fileName[MAX_PATH] = {0};

    glm::mat4 userMatrix = glm::mat4(1);

    HWND statusWnd;
    ViewportWindow mainViewport;

    const TCHAR * className() const { return APP_NAME; }

    void pushUndo();
    void pushUndo(EditorState newState);
    void updateStatus();
    void flashSel();
    void showError(winged_error err);
    void saveAs();

    BOOL onCreate(HWND, LPCREATESTRUCT);
    void onNCDestroy(HWND);
    void onSize(HWND, UINT, int, int);
    void onCommand(HWND, int, HWND, UINT);
    void onInitMenu(HWND, HMENU);
    void onMenuSelect(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;
};


extern MainWindow g_mainWindow;
extern EditorState g_state;
extern PickResult g_hover;
extern face_id g_hoverFace;

extern Tool g_tool;
extern glm::vec3 g_moved;
extern std::vector<glm::vec3> g_drawVerts;

extern bool g_flashSel;

size_t numDrawPoints();

} // namespace
