// The main window hosts the menu, toolbar, status bar, and primary viewport,
// and handles most keyboard commands. Global editor state is also defined here.

#pragma once
#include "common.h"

#include <vector>
#include <stack>
#include <memory>
#include <unordered_set>
#include "winchroma.h"
#include "editor.h"
#include "library.h"
#include "viewport.h"
#include "rendermesh.h"

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
    ViewportWindow *activeViewport, *hoveredViewport;

    void pushUndo();
    void pushUndo(EditorState newState);
    void undo();
    void updateStatus();
    void invalidateRenderMesh();
    void refreshAll();
    void refreshAllImmediate();
    void flashSel();
    void showError(winged_error err);
    void showStdException(std::exception e);
    bool removeViewport(ViewportWindow *viewport);
    void open(const TCHAR *path);
    bool promptSaveChanges();

private:
    std::stack<EditorState> undoStack;
    std::stack<EditorState> redoStack;
    int unsavedCount = 0;
    TCHAR filePath[MAX_PATH] = L"", objFilePath[MAX_PATH] = L"";

    glm::mat3 userMatrix = glm::mat3(1);
    glm::mat3 userPaintMatrix = glm::mat3(1);

    HWND toolbarWnd, statusWnd;
    ViewportWindow mainViewport;
    std::unordered_set<std::unique_ptr<ViewportWindow>> extraViewports;

    void setTool(Tool tool);
    void closeExtraViewports();
    void resetModel();
    bool save();
    bool saveAs();

    BOOL onCreate(HWND, LPCREATESTRUCT);
    void onClose(HWND);
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
extern Library g_library;
extern PickResult g_hover;
extern face_id g_hoverFace;

extern Tool g_tool;
extern std::vector<glm::vec3> g_drawVerts;

extern RenderMesh g_renderMesh;
extern bool g_renderMeshDirty;
extern bool g_flashSel;

size_t numDrawPoints();

} // namespace
