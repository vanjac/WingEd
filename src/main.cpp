#include "main.h"
#include <glm/gtc/matrix_transform.hpp>
#include "ops.h"
#include "file.h"
#include "image.h"
#include "mathutil.h"
#include "resource.h"
#include <immer/set_transient.hpp>
#include <shlwapi.h>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Rpcrt4.lib")
#pragma comment(lib, "Opengl32.lib")
#pragma comment(lib, "Glu32.lib")
#pragma comment(lib, "Gdiplus.lib")

using namespace chroma;

namespace winged {

enum ToolbarImage {
    TIMG_ELEMENTS, TIMG_SOLIDS, TIMG_SELECT, TIMG_POLYGON, TIMG_KNIFE, TIMG_JOIN, TIMG_GRID,
    TIMG_ERASE, TIMG_EXTRUDE, TIMG_SPLIT, TIMG_DUPLICATE, TIMG_FLIP, TIMG_SNAP,
    NUM_TOOLBAR_IMAGES
};
enum StatusPart {
    STATUS_GRID, STATUS_SELECT, STATUS_DIMEN, STATUS_HELP, NUM_STATUS_PARTS
};

// main.h
MainWindow g_mainWindow;
EditorState g_state;
Library g_library;
PickResult g_hover;
face_id g_hoverFace = {};
Tool g_tool = TOOL_SELECT;
std::vector<glm::vec3> g_drawVerts;
RenderMesh g_renderMesh;
bool g_renderMeshDirty = true;
bool g_flashSel = false;


size_t numDrawPoints() {
    if (g_tool == TOOL_KNIFE) {
        if (g_state.selVerts.size() == 1)
            return g_drawVerts.size() + 1;
        else
            return 0;
    } else if (TOOL_FLAGS[g_tool] & TOOLF_DRAW) {
        return g_drawVerts.size();
    } else {
        return 0;
    }
}

static void resetToolState() {
    g_drawVerts.clear();
}

static std::vector<edge_id> sortEdgeLoop(const Surface &surf, immer::set<edge_id> edges) {
    std::vector<edge_id> loop;
    auto edgesTrans = edges.transient();
    loop.reserve(edges.size());
    loop.push_back(*edges.begin());
    edgesTrans.erase(loop[0]);
    while (loop.size() != edges.size()) {
        auto nextVert = loop.back().in(surf).next.in(surf).vert;
        edge_id foundEdge = {};
        for (const auto &e : edgesTrans) {
            const auto &edge = e.in(surf);
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

static EditorState erase(EditorState state) {
    EditorState newState = state;
    if (state.selMode == SEL_ELEMENTS) {
        // edges first, then vertices
        bool anyDeleted = false;
        for (const auto &e : state.selEdges) {
            if (e.find(newState.surf)) { // could have been deleted previously
                newState.surf = mergeFaces(std::move(newState.surf), e);
                anyDeleted = true;
            }
        }
        for (const auto &v : state.selVerts) {
            if (auto vert = v.find(newState.surf)) {
                // make sure vert has only two edges
                const auto &edge = vert->edge.in(newState.surf);
                const auto &twin = edge.twin.in(newState.surf);
                const auto &twinNext = twin.next.in(newState.surf);
                if (twinNext.twin.in(newState.surf).next == vert->edge) {
                    newState.surf = joinVerts(std::move(newState.surf), edge.prev, vert->edge);
                    anyDeleted = true;
                }
            }
        }
        if (!anyDeleted)
            throw winged_error();
    } else if (state.selMode == SEL_SOLIDS) {
        for (const auto &v : state.selVerts)
            newState.surf.verts = std::move(newState.surf.verts).erase(v);
        for (const auto &f : state.selFaces)
            newState.surf.faces = std::move(newState.surf.faces).erase(f);
        for (const auto &e : state.selEdges)
            newState.surf.edges = std::move(newState.surf.edges).erase(e)
                .erase(e.in(state.surf).twin);
    } else {
        throw winged_error();
    }
    return newState;
}

#ifdef CHROMA_DEBUG
static const HEdge expectSingleSelEdge() {
    if (g_state.selEdges.size() == 1)
        return g_state.selEdges.begin()->in(g_state.surf);
    throw winged_error(L"No selected edge");
}
#endif

INT_PTR CALLBACK matrixDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtr(dlg, DWLP_USER, lParam);
            const auto &mat = *reinterpret_cast<glm::mat3 *>(lParam);
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
                    glm::mat3 &mat = *reinterpret_cast<glm::mat3 *>(
                        GetWindowLongPtr(dlg, DWLP_USER));
                    for (int i = 0; i < 9; i++) {
                        TCHAR buf[64];
                        GetDlgItemText(dlg, 1000 + i, buf, _countof(buf));
                        mat[i % 3][i / 3] = float(_tstof(buf));
                    }
                    EndDialog(dlg, LOWORD(wParam));
                    return true;
                }
            }
            break;
    }
    return false;
}

void MainWindow::pushUndo() {
    undoStack.push(g_state);
    redoStack = {};
    unsavedCount++;
}

void MainWindow::pushUndo(EditorState newState) {
    validateSurface(newState.surf);
    pushUndo();
    g_state = cleanSelection(newState);
}

void MainWindow::undo() {
    if (!undoStack.empty()) {
        redoStack.push(g_state);
        g_state = undoStack.top();
        undoStack.pop();
        unsavedCount--;
    }
    resetToolState();
}

void MainWindow::updateStatus() {
    TCHAR buf[256];

    TCHAR *str = buf;
    if (unsavedCount)
        str += _stprintf(str, L"* ");
    if (filePath[0] == 0) {
        str += _stprintf(str, L"Untitled");
    } else {
        str += _stprintf(str, L"%s", PathFindFileName(filePath));
    }
    str += _stprintf(str, L" - %s", APP_NAME);
    SendMessage(wnd, WM_SETTEXT, 0, LRESULT(buf));

    _stprintf(buf, L"Grid:  %g", g_state.gridSize);
    SendMessage(statusWnd, SB_SETTEXT, STATUS_GRID, LPARAM(buf));

    buf[0] = 0;
    str = buf;
    if (!g_state.selVerts.empty())
        str += _stprintf(str, L"%zd vert ", g_state.selVerts.size());
    if (!g_state.selEdges.empty())
        str += _stprintf(str, L"%zd edge ", g_state.selEdges.size());
    if (!g_state.selFaces.empty())
        str += _stprintf(str, L"%zd face", g_state.selFaces.size());
    SendMessage(statusWnd, SB_SETTEXT, STATUS_SELECT, LPARAM(buf));

    if (g_tool == TOOL_SELECT && activeViewport->mouseMode == MOUSE_TOOL) {
        auto moved = activeViewport->moved;
        _stprintf(buf, L"Move  %.3g, %.3g, %.3g", VEC3_ARGS(moved));
    } else if (numDrawPoints() > 0 && g_hover.type) {
        auto lastPos = g_drawVerts.empty()
            ? g_state.selVerts.begin()->in(g_state.surf).pos
            : g_drawVerts.back();
        _stprintf(buf, L"Len:  %g", glm::distance(lastPos, g_hover.point));
    } else if (g_state.selEdges.size() == 1) {
        const auto &edge = g_state.selEdges.begin()->in(g_state.surf);
        auto v1 = edge.vert.in(g_state.surf).pos;
        auto v2 = edge.twin.in(g_state.surf).vert.in(g_state.surf).pos;
        _stprintf(buf, L"Len:  %g", glm::distance(v1, v2));
    } else if (g_state.selVerts.size() == 1) {
        auto pos = g_state.selVerts.begin()->in(g_state.surf).pos;
        _stprintf(buf, L"Pos:  %.3g, %.3g, %.3g", VEC3_ARGS(pos));
    } else {
        buf[0] = 0;
    }
    SendMessage(statusWnd, SB_SETTEXT, STATUS_DIMEN, LPARAM(buf));

    const TCHAR *helpText = L"";
    if (activeViewport->mouseMode == MOUSE_CAM_ROTATE) {
        if (activeViewport->view.mode == VIEW_FLY)
            helpText = L"Drag: Look";
        else
            helpText = L"Drag: Orbit";
    } else if (activeViewport->mouseMode == MOUSE_CAM_PAN) {
        if (activeViewport->view.mode == VIEW_FLY)
            helpText = L"Drag: Move   Shift: Pan";
        else
            helpText = L"Drag: Pan   Shift: Dolly";
    } else {
        switch (g_tool) {
            case TOOL_SELECT:
                if (activeViewport->mouseMode == MOUSE_TOOL)
                    helpText = L"Shift: Snap axis   Ctrl: Orthogonal";
                else
                    helpText = L"Click: Select   Shift: Toggle"
                        L"   Drag: Move   Alt-Drag: Move on face plane";
                break;
            case TOOL_POLY:
                if (g_hover.type == PICK_DRAWVERT && g_hover.val == 0)
                    helpText = L"Click: Complete polygon   Shift-click: Stay in tool";
                else
                    helpText = L"Click: Add point   Bksp: Delete point";
                break;
            case TOOL_KNIFE:
                helpText = L"Click: Add vertex   Bksp: Delete vertex   Alt: Ignore vertices/edges";
                break;
            case TOOL_JOIN:
                if (hasSelection(g_state))
                    helpText = L"Click: Join with selection   Shift-click: Stay in tool";
                else
                    helpText = L"Click: Select";
                break;
            default:
                break;
        }
    }
    SendMessage(statusWnd, SB_SETTEXT, STATUS_HELP, LPARAM(helpText));

    auto menu = GetMenu(wnd);
    onInitMenu(wnd, menu);
    updateToolbarStates(toolbarWnd, menu);
}

void MainWindow::refreshAll() {
    g_renderMeshDirty = true;
    mainViewport.invalidateRenderMesh();
    mainViewport.refresh();
    for (const auto &viewport : extraViewports) {
        viewport->invalidateRenderMesh();
        viewport->refresh();
    }
}

void MainWindow::refreshAllImmediate() {
    g_renderMeshDirty = true;
    mainViewport.invalidateRenderMesh();
    mainViewport.refreshImmediate();
    for (const auto &viewport : extraViewports) {
        viewport->invalidateRenderMesh();
        viewport->refreshImmediate();
    }
}

void MainWindow::flashSel() {
    g_flashSel = true;
    refreshAllImmediate();
    Sleep(200);
    g_flashSel = false;
    refreshAll();
}

void MainWindow::showError(winged_error const& err) {
    if (err.message) {
        MessageBox(wnd, err.message, APP_NAME, MB_ICONERROR);
    } else {
        MessageBeep(MB_OK);
        auto prevCursor = SetCursor(LoadCursor(NULL, IDC_NO));
        Sleep(300);
        SetCursor(prevCursor);
    }
}

void MainWindow::showStdException(std::exception const& e) {
    MessageBoxA(wnd, e.what(), "Unexpected Error", MB_ICONERROR);
}

void MainWindow::updateHover() {
    auto pt = cursorPos();
    if (hoveredViewport && WindowFromPoint(pt) == hoveredViewport->wnd) {
        hoveredViewport->updateHover(screenToClient(hoveredViewport->wnd, pt));
        setCursorHitTest(hoveredViewport->wnd, pt);
    }
}

void MainWindow::setSelMode(SelectMode mode) {
    g_state.selMode = mode;
    if (!(TOOL_FLAGS[g_tool] & (1 << mode)))
        setTool(TOOL_SELECT);
    else
        updateHover();
}

void MainWindow::setTool(Tool tool) {
    g_tool = tool;
    resetToolState();
    if (!(TOOL_FLAGS[tool] & (1 << g_state.selMode)))
        g_state.selMode = SEL_ELEMENTS; // TODO
    if ((TOOL_FLAGS[tool] & TOOLF_DRAW) && (TOOL_FLAGS[tool] & TOOLF_HOVFACE))
        if (auto face = g_hoverFace.find(g_state.surf))
            g_state.workPlane = facePlane(g_state.surf, *face);
    updateHover();
}

bool MainWindow::removeViewport(ViewportWindow *viewport) {
    if (activeViewport == viewport)
        activeViewport = &mainViewport;
    if (hoveredViewport == viewport)
        hoveredViewport = NULL;
    // hack https://stackoverflow.com/a/60220391/11525734
    std::unique_ptr<ViewportWindow> stalePtr(viewport);
    auto ret = extraViewports.erase(stalePtr);
    stalePtr.release();
    return ret;
}

void MainWindow::closeExtraViewports() {
    activeViewport = &mainViewport;
    for (const auto &viewport : extraViewports)
        viewport->destroy();
    extraViewports.clear();
}

void MainWindow::resetModel() {
    undoStack = {};
    redoStack = {};
    unsavedCount = 0;
    objFilePath[0] = 0;
    resetToolState();

    closeExtraViewports();
    mainViewport.clearTextureCache();
    mainViewport.updateProjMat();
}

void MainWindow::open(const TCHAR *path) {
    auto res = readFile(path, g_library.rootPath.c_str());
    validateSurface(std::get<0>(res).surf);
    tie(g_state, mainViewport.view, g_library) = std::move(res);
    memcpy(filePath, path, sizeof(filePath));
    resetModel();
}

bool MainWindow::saveAs() {
    auto filters = L"WingEd File (.wing)\0*.wing\0All Files\0*.*\0\0";
    if (GetSaveFileName(tempPtr(makeOpenFileName(filePath, wnd, filters, L"wing")))) {
        writeFile(filePath, g_state, mainViewport.view, g_library);
        unsavedCount = 0;
        return true;
    }
    return false;
}

bool MainWindow::save() {
    if (!filePath[0]) {
        return saveAs();
    } else {
        writeFile(filePath, g_state, mainViewport.view, g_library);
        unsavedCount = 0;
        return true;
    }
}

bool MainWindow::promptSaveChanges() {
    if (unsavedCount) {
        auto name = (filePath[0] == 0) ? L"Untitled" : PathFindFileName(filePath);
        TCHAR buf[256];
        _stprintf(buf, L"Save changes to %s?", name);
        switch (MessageBox(wnd, buf, APP_NAME, MB_YESNOCANCEL)) {
            case IDYES:
                return save();
            case IDNO:
                return true;
            case IDCANCEL:
                return false;
        }
    }
    return true;
}

BOOL MainWindow::onCreate(HWND, LPCREATESTRUCT) {
    mainViewport.createChild(wnd);
    activeViewport = &mainViewport;

    TBBUTTON buttons[] = {
        {TIMG_ELEMENTS,     IDM_SEL_ELEMENTS,   TBSTATE_ENABLED, BTNS_BUTTON},
        {TIMG_SOLIDS,       IDM_SEL_SOLIDS,     TBSTATE_ENABLED, BTNS_BUTTON},
        {0, 0, 0, BTNS_SEP},
        {TIMG_SELECT,       IDM_TOOL_SELECT,    TBSTATE_ENABLED, BTNS_BUTTON},
        {TIMG_POLYGON,      IDM_TOOL_POLY,      TBSTATE_ENABLED, BTNS_BUTTON},
        {TIMG_KNIFE,        IDM_TOOL_KNIFE,     TBSTATE_ENABLED, BTNS_BUTTON},
        {TIMG_JOIN,         IDM_TOOL_JOIN,      TBSTATE_ENABLED, BTNS_BUTTON},
        {0, 0, 0, BTNS_SEP},
        {TIMG_GRID,         IDM_TOGGLE_GRID,    TBSTATE_ENABLED, BTNS_BUTTON},
        {0, 0, 0, BTNS_SEP},
        {TIMG_ERASE,        IDM_ERASE,          TBSTATE_ENABLED, BTNS_BUTTON},
        {TIMG_EXTRUDE,      IDM_EXTRUDE,        TBSTATE_ENABLED, BTNS_BUTTON},
        {TIMG_SPLIT,        IDM_SPLIT_LOOP,     TBSTATE_ENABLED, BTNS_BUTTON},
        {TIMG_DUPLICATE,    IDM_DUPLICATE,      TBSTATE_ENABLED, BTNS_BUTTON},
        {TIMG_FLIP,         IDM_FLIP_NORMALS,   TBSTATE_ENABLED, BTNS_BUTTON},
        {TIMG_SNAP,         IDM_SNAP,           TBSTATE_ENABLED, BTNS_BUTTON},
    };
    auto toolbarBmp = LoadBitmap(GetModuleHandle(NULL), MAKEINTRESOURCE(IDB_TOOLBAR));
    toolbarWnd = CreateToolbarEx(wnd,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | CCS_TOP | TBSTYLE_TOOLTIPS,
        1, NUM_TOOLBAR_IMAGES, NULL, UINT(size_t(toolbarBmp)), buttons, _countof(buttons),
        0, 0, 24, 24, sizeof(TBBUTTON));

    statusWnd = CreateStatusWindow(
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | CCS_BOTTOM | SBARS_SIZEGRIP,
        NULL, wnd, 0);
    int parts[NUM_STATUS_PARTS];
    int x = 0;
    x += 70; parts[STATUS_GRID] = x;
    x += 150; parts[STATUS_SELECT] = x;
    x += 150; parts[STATUS_DIMEN] = x;
    parts[NUM_STATUS_PARTS - 1] = -1;
    SendMessage(statusWnd, SB_SETPARTS, NUM_STATUS_PARTS, LPARAM(parts));
    updateStatus();

    return true;
}

void MainWindow::onClose(HWND) {
    if (promptSaveChanges()) {
        closeExtraViewports();
        mainViewport.destroy();
        FORWARD_WM_CLOSE(wnd, DefWindowProc);
    }
}

void MainWindow::onNCDestroy(HWND) {
    PostQuitMessage(0);
}

void MainWindow::onActivate(HWND, UINT state, HWND, BOOL minimized) {
    if (state && !minimized)
        activeViewport = &mainViewport;
}

void MainWindow::onSize(HWND, UINT, int cx, int cy) {
    SendMessage(toolbarWnd, TB_AUTOSIZE, 0, 0);
    auto toolbarHeight = rectHeight(windowRect(toolbarWnd));
    auto statusHeight = rectHeight(windowRect(statusWnd));
    MoveWindow(statusWnd, 0, cy - statusHeight, cx, statusHeight, true);
    MoveWindow(mainViewport.wnd, 0, toolbarHeight, cx, cy - toolbarHeight - statusHeight, true);
}

void MainWindow::onCommand(HWND, int id, HWND ctl, UINT code) {
    if (ctl && code != BN_CLICKED) return;

    if (mainViewport.onCommand(mainViewport.wnd, id, ctl, code))
        return;

    try {
        switch (id) {
            /* File */
            case IDM_NEW:
                if (promptSaveChanges()) {
                    g_state = {};
                    mainViewport.view = {};
                    g_library.clear();
                    filePath[0] = 0;
                    resetModel();
                }
                break;
            case IDM_OPEN: {
                TCHAR newFile[MAX_PATH] = L"";
                auto filters = L"WingEd File (.wing)\0*.wing\0\0";
                if (GetOpenFileName(tempPtr(makeOpenFileName(newFile, wnd, filters, L"wing")))) {
                    if (promptSaveChanges())
                        open(newFile);
                }
                break;
            }
            case IDM_SAVE_AS:
                saveAs();
                break;
            case IDM_SAVE:
                save();
                break;
            case IDM_EXPORT_OBJ: {
                if (!objFilePath[0] && filePath[0]) {
                    lstrcpy(objFilePath, filePath);
                    lstrcpy(PathFindExtension(objFilePath), L".obj");
                }
                TCHAR mtlFile[MAX_PATH] = L"";
                auto filters = L"OBJ file (.obj)\0*.obj\0All Files\0*.*\0\0";
                auto saveFile = makeOpenFileName(objFilePath, wnd, filters, L"obj");
                saveFile.lpstrFileTitle = mtlFile;
                saveFile.nMaxFileTitle = _countof(mtlFile);
                saveFile.lpstrTitle = L"Export OBJ";
                if (GetSaveFileName(&saveFile)) {
                    lstrcpy(PathFindExtension(mtlFile), L".mtl");
                    writeObj(objFilePath, g_state.surf, g_library, mtlFile, true);
                }
                break;
            }
            case IDM_ADD_TEXTURE: {
                TCHAR texFile[MAX_PATH] = L"";
                // https://learn.microsoft.com/en-us/windows/win32/gdiplus/-gdiplus-types-of-bitmaps-about
                auto filters =
                    L"Supported Images (.png, .jpg, .jpeg, .bmp, .gif, .tif, .tiff)\0"
                    "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
                    "All Files\0*.*\0\0";
                if (GetOpenFileName(tempPtr(makeOpenFileName(texFile, wnd, filters)))) {
                    auto texFileStr = std::wstring(texFile);
                    id_t texId = g_library.pathIds[texFileStr];
                    if (texId == id_t{}) {
                        texId = genId();
                        g_library.addFile(texId, texFileStr);
                    }

                    EditorState newState = g_state;
                    newState.surf = assignPaint(g_state.surf, g_state.selFaces, Paint{texId});
                    pushUndo(std::move(newState));
                }
                break;
            }
            case IDM_RELOAD_ASSETS:
                mainViewport.clearTextureCache();
                for (const auto &viewport : extraViewports)
                    viewport->clearTextureCache();
                break;
            /* Tool */
            case IDM_TOOL_SELECT:
                setTool(TOOL_SELECT);
                break;
            case IDM_TOOL_POLY:
                setTool(TOOL_POLY);
                if (g_state.selFaces.size() == 1) {
                    g_state.workPlane = facePlane(g_state.surf,
                        g_state.selFaces.begin()->in(g_state.surf));
                } else if (activeViewport->view.mode == VIEW_ORTHO) {
                    g_state.workPlane.norm = activeViewport->forwardAxis();
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
            case IDM_NEW_VIEWPORT: {
                const auto &newViewport = *extraViewports.emplace(new ViewportWindow).first;
                newViewport->view = activeViewport->view;
                RECT rect = defaultWindowRect(clientSize(activeViewport->wnd),
                    false, WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW);
                newViewport->create(APP_NAME, rect, WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW, wnd);
                ShowWindow(newViewport->wnd, SW_NORMAL);
                break;
            }
            /* Edit */
            case IDM_UNDO:
                undo();
                break;
            case IDM_REDO:
                if (!redoStack.empty()) {
                    undoStack.push(g_state);
                    g_state = redoStack.top();
                    redoStack.pop();
                    unsavedCount++;
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
                for (const auto &f : g_state.selFaces) {
                    immer::set_transient<edge_id> extEdges;
                    for (const auto &e : g_state.selEdges) {
                        const auto &edge = e.in(newState.surf);
                        if (edge.face == f)
                            extEdges.insert(e);
                        else if (edge.twin.in(newState.surf).face == f)
                            extEdges.insert(edge.twin);
                    }
                    newState.surf = extrudeFace(newState.surf, f, extEdges.persistent());
                    for (const auto &e : extEdges) {
                        auto primary = primaryEdge(e.pair(newState.surf));
                        newState.selEdges = std::move(newState.selEdges).insert(primary);
                    }
                }
                pushUndo(std::move(newState));
                flashSel();
                break;
            }
            case IDM_SPLIT_LOOP: {
                auto loop = sortEdgeLoop(g_state.surf, g_state.selEdges);
                EditorState newState = g_state;
                newState.surf = splitEdgeLoop(g_state.surf, loop);
                newState.selVerts = {};
                newState.selEdges = {};
                for (const auto &e : loop) {
                    edge_id primary = primaryEdge(e.pair(newState.surf));
                    newState.selEdges = std::move(newState.selEdges).insert(primary);
                }
                pushUndo(std::move(newState));
                flashSel();
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
            case IDM_TRANSFORM_MATRIX:
                if (DialogBoxParam(GetModuleHandle(NULL), L"IDD_MATRIX", wnd, matrixDlgProc,
                        LPARAM(&userMatrix)) == IDOK) {
                    auto verts = selAttachedVerts(g_state);
                    auto center = vertsCenter(g_state.surf, verts);
                    EditorState newState = g_state;
                    newState.surf = transformVertices(g_state.surf, verts, glm::translate(
                        glm::translate(glm::mat4(1), center) * glm::mat4(userMatrix), -center));
                    pushUndo(std::move(newState));
                }
                break;
            case IDM_PAINT_MATRIX:
                if (DialogBoxParam(GetModuleHandle(NULL), L"IDD_MATRIX", wnd, matrixDlgProc,
                        LPARAM(&userPaintMatrix)) == IDOK) {
                    EditorState newState = g_state;
                    auto mat = glm::inverse(userPaintMatrix);
                    newState.surf = transformPaint(g_state.surf, g_state.selFaces, mat);
                    pushUndo(std::move(newState));
                }
                break;
            case IDM_MARK_HOLE:
                EditorState newState = g_state;
                auto paint = Paint{Paint::HOLE_MATERIAL};
                newState.surf = assignPaint(g_state.surf, g_state.selFaces, paint);
                pushUndo(std::move(newState));
                break;
        }
    } catch (winged_error const& err) {
        showError(err);
#ifndef CHROMA_DEBUG
    } catch (std::exception const& e) {
        showStdException(e);
#endif
    }
    updateStatus();
    refreshAll();
}

void MainWindow::onInitMenu(HWND, HMENU menu) {
    auto hasSel = hasSelection(g_state);
    auto selElem = (g_state.selMode == SEL_ELEMENTS);
    auto selSolid = (g_state.selMode == SEL_SOLIDS);
    EnableMenuItem(menu, IDM_CLEAR_SELECT, (hasSel || numDrawPoints() > 0) ?
        MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_UNDO, undoStack.empty() ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(menu, IDM_REDO, redoStack.empty() ? MF_GRAYED : MF_ENABLED);
    CheckMenuItem(menu, IDM_TOGGLE_GRID, g_state.gridOn ? MF_CHECKED : MF_UNCHECKED);
    EnableMenuItem(menu, IDM_ERASE, hasSel ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_EXTRUDE, (!g_state.selFaces.empty() && selElem) ?
        MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_SPLIT_LOOP, (!g_state.selEdges.empty() && selElem) ?
        MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_DUPLICATE, (hasSel && selSolid) ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_SNAP, hasSel ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_TRANSFORM_MATRIX, hasSel ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_PAINT_MATRIX, g_state.selFaces.empty() ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(menu, IDM_MARK_HOLE, g_state.selFaces.empty() ? MF_GRAYED : MF_ENABLED);
    CheckMenuItem(menu, IDM_WIREFRAME, (mainViewport.view.showElem & PICK_FACE) ?
        MF_UNCHECKED : MF_CHECKED);
    EnableMenuItem(menu, IDM_FOCUS, hasSel ? MF_ENABLED : MF_GRAYED);

    MENUITEMINFO selMenu = {sizeof(selMenu), MIIM_SUBMENU};
    GetMenuItemInfo(menu, IDM_SEL_MENU, false, &selMenu);
    CheckMenuRadioItem(selMenu.hSubMenu, 0, NUM_SELMODES - 1, g_state.selMode, MF_BYPOSITION);

    MENUITEMINFO toolMenu = {sizeof(toolMenu), MIIM_SUBMENU};
    GetMenuItemInfo(menu, IDM_TOOL_MENU, false, &toolMenu);
    CheckMenuRadioItem(toolMenu.hSubMenu, 0, NUM_TOOLS - 1, g_tool, MF_BYPOSITION);

    MENUITEMINFO viewMenu = {sizeof(viewMenu), MIIM_SUBMENU};
    GetMenuItemInfo(menu, IDM_VIEW_MENU, false, &viewMenu);
    CheckMenuRadioItem(viewMenu.hSubMenu, 0, NUM_VIEWMODES - 1,
        mainViewport.view.mode, MF_BYPOSITION);
}

void MainWindow::onMenuSelect(HWND, UINT msg, WPARAM wParam, LPARAM lParam) {
    MenuHelp(msg, wParam, lParam, NULL, GetModuleHandle(NULL), statusWnd, tempPtr(0u));
}

void MainWindow::onMeasureItem(HWND, MEASUREITEMSTRUCT *measure) {
    // hack to draw classic-style menus
    measure->itemWidth = 0;
    measure->itemHeight = 0;
}

LRESULT MainWindow::onNotify(HWND, int, NMHDR *nmHdr) {
    if (nmHdr->code == TTN_GETDISPINFO)
        handleToolbarTip(reinterpret_cast<NMTTDISPINFO *>(nmHdr), GetMenu(wnd));
    return 0;
}

LRESULT MainWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(wnd, WM_CREATE, onCreate);
        HANDLE_MSG(wnd, WM_CLOSE, onClose);
        HANDLE_MSG(wnd, WM_NCDESTROY, onNCDestroy);
        HANDLE_MSG(wnd, WM_ACTIVATE, onActivate);
        HANDLE_MSG(wnd, WM_SIZE, onSize);
        HANDLE_MSG(wnd, WM_COMMAND, onCommand);
        HANDLE_MSG(wnd, WM_INITMENU, onInitMenu);
        case WM_MENUSELECT: onMenuSelect(wnd, msg, wParam, lParam); return 0;
        HANDLE_MSG(wnd, WM_MEASUREITEM, onMeasureItem);
        HANDLE_MSG(wnd, WM_NOTIFY, onNotify);
    }
    return DefWindowProc(wnd, msg, wParam, lParam);
}

} // namespace

using namespace winged;

int APIENTRY _tWinMain(HINSTANCE instance, HINSTANCE, LPTSTR, int showCmd) {
    if (!initViewport())
        return 0;
    initImage();
    WNDCLASSEX mainClass = makeClass(APP_NAME, windowImplProc);
    mainClass.lpszMenuName = APP_NAME;
    RegisterClassEx(&mainClass);
    auto wnd = g_mainWindow.create(APP_NAME, defaultWindowRect({640, 480}, true));
    if (!wnd) return -1;
    ShowWindow(wnd, showCmd);
    auto mainAccel = LoadAccelerators(instance, L"Accel");
    auto viewAccel = LoadAccelerators(instance, L"ViewAccel");
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (TranslateAccelerator(wnd, mainAccel, &msg)
                || TranslateAccelerator(g_mainWindow.activeViewport->wnd, viewAccel, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    uninitImage();
    return int(msg.wParam);
}

CHROMA_MAIN
