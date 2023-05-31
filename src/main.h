#pragma once
#include "common.h"

#include <vector>
#include <stack>
#include <unordered_set>
#include "winchroma.h"
#include "editor.h"
#include "viewport.h"
#include "resource.h"

namespace winged {

const TCHAR APP_NAME[] = _T("WingEd");

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
const ToolFlags TOOL_FLAGS[] = {
    /*select*/  TOOLF_ALLSEL,
    /*poly*/    TOOLF_ELEMENTS | TOOLF_DRAW,
    /*knife*/   TOOLF_ELEMENTS | TOOLF_DRAW | TOOLF_HOVFACE,
    /*join*/    TOOLF_ELEMENTS | TOOLF_HOVFACE,
};

const PickType
    PICK_WORKPLANE = 0x8,
    PICK_DRAWVERT = 0x10;

class MainWindow : public chroma::WindowImpl {
    const TCHAR * className() const override { return APP_NAME; }

public:
    ViewportWindow *activeViewport;

    void pushUndo();
    void pushUndo(EditorState newState);
    void updateStatus();
    void refreshAll();
    void refreshAllImmediate();
    void flashSel();
    void showError(winged_error err);
    bool removeViewport(ViewportWindow *viewport);

private:
    std::stack<EditorState> undoStack;
    std::stack<EditorState> redoStack;
    TCHAR fileName[MAX_PATH] = {0};

    glm::mat4 userMatrix = glm::mat4(1);

    HWND toolbarWnd, statusWnd;
    ViewportWindow mainViewport;
    std::unordered_set<ViewportWindow *> extraViewports;

    void closeExtraViewports();
    void saveAs();

    BOOL onCreate(HWND, LPCREATESTRUCT);
    void onDestroy(HWND);
    void onNCDestroy(HWND);
    void onActivate(HWND, UINT, HWND, BOOL);
    void onSize(HWND, UINT, int, int);
    void onCommand(HWND, int, HWND, UINT);
    void onInitMenu(HWND, HMENU);
    void onMenuSelect(HWND, UINT, WPARAM, LPARAM);
    void onMeasureItem(HWND, MEASUREITEMSTRUCT *);
    LRESULT onNotify(HWND, int, NMHDR *);
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
