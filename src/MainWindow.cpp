#include "MainWindow.h"
#include "ReportGenerator.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

namespace {
    constexpr wchar_t kClassName[] = L"WinDiagProMainWindow";
    constexpr UINT WM_APP_RESULTS = WM_APP + 1;
    constexpr UINT WM_APP_REPAIRLOG = WM_APP + 101;

    enum TabIndex {
        TAB_DASHBOARD = 0,
        TAB_NETWORK,
        TAB_SYSTEM,
        TAB_HARDWARE,
        TAB_SECURITY,
        TAB_TOPOLOGY,
        TAB_REPAIR,
        TAB_REPORT,
        TAB_HELP,
        TAB_COUNT
    };

    enum ControlId {
        ID_TAB = 1000,
        ID_STATUS,
        ID_LV_DASHBOARD, ID_ED_DIAGNOSIS, ID_BTN_FULLSCAN,
        ID_LV_NETWORK, ID_BTN_NETSCAN, ID_BTN_TRACERT,
        ID_LV_SYSTEM, ID_BTN_SYSSCAN, ID_BTN_SFC, ID_BTN_DISM_CHECK, ID_BTN_DISM_SCAN,
        ID_LV_HARDWARE, ID_BTN_HWSCAN,
        ID_LV_SECURITY, ID_BTN_SECSCAN,
        ID_TOPOLOGY_VIEW,
        ID_LV_REPAIR, ID_ED_REPAIRLOG, ID_BTN_RUNREPAIR,
        ID_ED_REPORT, ID_BTN_SAVE_TXT, ID_BTN_SAVE_HTML, ID_BTN_SAVE_MD, ID_BTN_SAVE_JSON,
        ID_BTN_OPEN_LAST_REPORT, ID_BTN_OPEN_LAST_REPORT_FOLDER,
        ID_ED_HELP, ID_BTN_OPEN_CMD, ID_BTN_OPEN_PS,
    };

    constexpr UINT_PTR ID_TOPOLOGY_TIMER = 1;

    // Context-menu command IDs (used with TrackPopupMenu's TPM_RETURNCMD).
    constexpr UINT CTX_COPY_SELECTED = 1;
    constexpr UINT CTX_COPY_ALL = 2;
    constexpr UINT CTX_QUICKACTION_BASE = 100;
}

int RunGui(HINSTANCE hInstance, int nCmdShow) {
    MainWindow win;
    if (!win.Create(hInstance, nCmdShow)) return 1;
    return win.RunMessageLoop();
}

MainWindow::MainWindow() {}

MainWindow::~MainWindow() {
    if (m_worker.joinable()) m_worker.join();
    if (m_uiFont) DeleteObject(m_uiFont);
}

LRESULT CALLBACK MainWindow::WndProcStatic(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            OnCreate(hwnd);
            return 0;
        case WM_SIZE:
            OnSize(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wparam), reinterpret_cast<HWND>(lparam));
            return 0;
        case WM_NOTIFY:
            OnNotify(lparam);
            return 0;
        case WM_CONTEXTMENU: {
            // Sent to the parent when a child control (ListView) doesn't
            // handle it itself - standard pattern for list-view context menus.
            HWND target = reinterpret_cast<HWND>(wparam);
            POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            if (pt.x == -1 && pt.y == -1) {
                // Invoked via keyboard (Shift+F10 / Menu key) - anchor near the focused row.
                int idx = ListView_GetNextItem(target, -1, LVNI_FOCUSED);
                RECT rc{ 0, 0, 0, 0 };
                if (idx >= 0) ListView_GetItemRect(target, idx, &rc, LVIR_BOUNDS);
                POINT client{ rc.left, rc.bottom };
                ClientToScreen(target, &client);
                pt = client;
            }
            ShowListContextMenu(target, pt);
            return 0;
        }
        case WM_APP_RESULTS:
            OnResultsReady();
            return 0;
        case WM_APP_REPAIRLOG:
            OnRepairLogReady();
            return 0;
        case WM_TIMER:
            if (wparam == ID_TOPOLOGY_TIMER) RefreshTopology();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, ID_TOPOLOGY_TIMER);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc),
        ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS |
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WndProcStatic;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return false;

    m_engine.Initialize();
    m_repairCatalog = RepairEngine::GetCatalog();

    m_hwnd = CreateWindowExW(0, kClassName, L"WinDiagPro - Offline Windows Troubleshooter",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760,
                              nullptr, nullptr, hInstance, this);
    if (!m_hwnd) return false;

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    // Kick off a fast, safe scan automatically so the dashboard isn't empty on launch.
    StartFullScan();
    return true;
}

int MainWindow::RunMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------
static HWND MakeListView(HWND parent, int id, HINSTANCE hInst) {
    // No LVS_SINGLESEL: multi-row selection is needed so "Copy Selected Row(s)"
    // can grab more than one line at a time (Shift/Ctrl+click, like Explorer).
    HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                               WS_CHILD | LVS_REPORT | WS_TABSTOP,
                               0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    const wchar_t* headers[] = { L"Status", L"Category", L"Severity", L"Check", L"Details", L"Recommendation" };
    int widths[] = { 60, 90, 70, 220, 340, 260 };
    for (int i = 0; i < 6; ++i) {
        col.pszText = const_cast<LPWSTR>(headers[i]);
        col.cx = widths[i];
        ListView_InsertColumn(lv, i, &col);
    }
    return lv;
}

static HWND MakeButton(HWND parent, int id, const wchar_t* text, HINSTANCE hInst) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
}

static HWND MakeReadOnlyEdit(HWND parent, int id, HINSTANCE hInst) {
    // Standard EDIT control: even read-only, this natively supports mouse
    // selection, Ctrl+A/Ctrl+C, and a right-click Copy context menu - no
    // extra code needed for the diagnosis/report/repair-log text areas.
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                            0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
}

void MainWindow::OnCreate(HWND hwnd) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

    m_uiFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hTab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_TAB, hInst, nullptr);
    const wchar_t* tabNames[TAB_COUNT] = {
        L"Dashboard", L"Network", L"System", L"Hardware", L"Security", L"Topology", L"Repair", L"Report", L"Help"
    };
    for (int i = 0; i < TAB_COUNT; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(tabNames[i]);
        TabCtrl_InsertItem(m_hTab, i, &item);
    }

    // Dashboard tab
    m_btnFullScan = MakeButton(hwnd, ID_BTN_FULLSCAN, L"Run Full Diagnostic", hInst);
    m_edDiagnosis = MakeReadOnlyEdit(hwnd, ID_ED_DIAGNOSIS, hInst);
    m_lvDashboard = MakeListView(hwnd, ID_LV_DASHBOARD, hInst);

    // Network tab
    m_btnNetScan = MakeButton(hwnd, ID_BTN_NETSCAN, L"Run Network Diagnostic", hInst);
    m_btnTracert = MakeButton(hwnd, ID_BTN_TRACERT, L"Trace Route to Internet (slow)", hInst);
    m_lvNetwork = MakeListView(hwnd, ID_LV_NETWORK, hInst);

    // System tab
    m_btnSysScan = MakeButton(hwnd, ID_BTN_SYSSCAN, L"Run System Checks", hInst);
    m_btnSfc = MakeButton(hwnd, ID_BTN_SFC, L"Run SFC Scan (slow)", hInst);
    m_btnDismCheck = MakeButton(hwnd, ID_BTN_DISM_CHECK, L"DISM CheckHealth", hInst);
    m_btnDismScan = MakeButton(hwnd, ID_BTN_DISM_SCAN, L"DISM ScanHealth (slow)", hInst);
    m_lvSystem = MakeListView(hwnd, ID_LV_SYSTEM, hInst);

    // Hardware tab
    m_btnHwScan = MakeButton(hwnd, ID_BTN_HWSCAN, L"Run Hardware Checks", hInst);
    m_lvHardware = MakeListView(hwnd, ID_LV_HARDWARE, hInst);

    // Security tab
    m_btnSecScan = MakeButton(hwnd, ID_BTN_SECSCAN, L"Run Security Checks", hInst);
    m_lvSecurity = MakeListView(hwnd, ID_LV_SECURITY, hInst);

    // Topology tab - draws its own fonts internally, not part of the shared UI font pass below.
    m_topologyView = CreateTopologyView(hwnd, hInst, ID_TOPOLOGY_VIEW);

    // Repair tab
    m_lvRepairCatalog = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | LVS_REPORT | WS_TABSTOP,
                                         0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LV_REPAIR, hInst, nullptr);
    ListView_SetExtendedListViewStyle(m_lvRepairCatalog, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);
    {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<LPWSTR>(L"Repair Action"); col.cx = 220;
        ListView_InsertColumn(m_lvRepairCatalog, 0, &col);
        col.pszText = const_cast<LPWSTR>(L"Description"); col.cx = 480;
        ListView_InsertColumn(m_lvRepairCatalog, 1, &col);
        col.pszText = const_cast<LPWSTR>(L"Reboot?"); col.cx = 80;
        ListView_InsertColumn(m_lvRepairCatalog, 2, &col);
        for (size_t i = 0; i < m_repairCatalog.size(); ++i) {
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = (int)i;
            it.pszText = const_cast<LPWSTR>(m_repairCatalog[i].name.c_str());
            ListView_InsertItem(m_lvRepairCatalog, &it);
            ListView_SetItemText(m_lvRepairCatalog, (int)i, 1, const_cast<LPWSTR>(m_repairCatalog[i].description.c_str()));
            ListView_SetItemText(m_lvRepairCatalog, (int)i, 2, const_cast<LPWSTR>(m_repairCatalog[i].requiresReboot ? L"Yes" : L"No"));
        }
    }
    m_btnRunRepair = MakeButton(hwnd, ID_BTN_RUNREPAIR, L"Run Checked Repairs", hInst);
    m_edRepairLog = MakeReadOnlyEdit(hwnd, ID_ED_REPAIRLOG, hInst);

    // Report tab
    m_edReport = MakeReadOnlyEdit(hwnd, ID_ED_REPORT, hInst);
    m_btnSaveTxt = MakeButton(hwnd, ID_BTN_SAVE_TXT, L"Save as .txt", hInst);
    m_btnSaveHtml = MakeButton(hwnd, ID_BTN_SAVE_HTML, L"Save as .html", hInst);
    m_btnSaveMd = MakeButton(hwnd, ID_BTN_SAVE_MD, L"Save as .md", hInst);
    m_btnSaveJson = MakeButton(hwnd, ID_BTN_SAVE_JSON, L"Save as .json", hInst);
    m_btnOpenLastReport = MakeButton(hwnd, ID_BTN_OPEN_LAST_REPORT, L"Open Last Saved Report", hInst);
    m_btnOpenLastReportFolder = MakeButton(hwnd, ID_BTN_OPEN_LAST_REPORT_FOLDER, L"Open Containing Folder", hInst);

    // Help tab - static, curated content baked into the EXE (see HelpContent.h).
    // Available with zero network access, unlike Microsoft's own online
    // troubleshooters - this is the whole point of the project.
    m_edHelp = MakeReadOnlyEdit(hwnd, ID_ED_HELP, hInst);
    SetWindowTextW(m_edHelp, GetOfflineHelpText().c_str());
    // A safety valve: if you need to go deeper than this app, an elevated
    // terminal is one click away rather than hunting through a Start menu
    // that might itself be misbehaving.
    m_btnOpenCmd = MakeButton(hwnd, ID_BTN_OPEN_CMD, L"Open Command Prompt (Admin)", hInst);
    m_btnOpenPs = MakeButton(hwnd, ID_BTN_OPEN_PS, L"Open PowerShell (Admin)", hInst);

    // Status bar
    m_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_STATUS, hInst, nullptr);
    m_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_MARQUEE,
                                   0, 0, 0, 0, hwnd, nullptr, hInst, nullptr);

    // Apply the UI font to everything.
    HWND kids[] = { m_hTab, m_btnFullScan, m_edDiagnosis, m_lvDashboard, m_btnNetScan, m_btnTracert, m_lvNetwork,
                     m_btnSysScan, m_btnSfc, m_btnDismCheck, m_btnDismScan, m_lvSystem,
                     m_btnHwScan, m_lvHardware, m_btnSecScan, m_lvSecurity,
                     m_lvRepairCatalog, m_btnRunRepair, m_edRepairLog,
                     m_edReport, m_btnSaveTxt, m_btnSaveHtml, m_btnSaveMd, m_btnSaveJson,
                     m_btnOpenLastReport, m_btnOpenLastReportFolder, m_edHelp,
                     m_btnOpenCmd, m_btnOpenPs, m_hStatus };
    for (HWND k : kids) if (k) SendMessageW(k, WM_SETFONT, (WPARAM)m_uiFont, TRUE);

    SetBusy(false, L"Ready.");
    LayoutTab(TAB_DASHBOARD);
}

void MainWindow::OnSize(int width, int height) {
    if (!m_hTab) return;

    int statusHeight = 24;
    SetWindowPos(m_hStatus, nullptr, 0, height - statusHeight, width, statusHeight, SWP_NOZORDER);
    SetWindowPos(m_hProgress, nullptr, width - 160, height - statusHeight + 3, 150, statusHeight - 6, SWP_NOZORDER);

    SetWindowPos(m_hTab, nullptr, 0, 0, width, height - statusHeight, SWP_NOZORDER);

    int idx = TabCtrl_GetCurSel(m_hTab);
    LayoutTab(idx < 0 ? 0 : idx);
}

void MainWindow::LayoutTab(int index) {
    RECT rc;
    GetClientRect(m_hTab, &rc);
    TabCtrl_AdjustRect(m_hTab, FALSE, &rc);
    int x = rc.left, y = rc.top, w = rc.right - rc.left, h = rc.bottom - rc.top;

    auto hideAllExcept = [&](std::initializer_list<HWND> visible) {
        HWND all[] = { m_btnFullScan, m_edDiagnosis, m_lvDashboard,
                        m_btnNetScan, m_btnTracert, m_lvNetwork,
                        m_btnSysScan, m_btnSfc, m_btnDismCheck, m_btnDismScan, m_lvSystem,
                        m_btnHwScan, m_lvHardware,
                        m_btnSecScan, m_lvSecurity,
                        m_topologyView,
                        m_lvRepairCatalog, m_btnRunRepair, m_edRepairLog,
                        m_edReport, m_btnSaveTxt, m_btnSaveHtml, m_btnSaveMd, m_btnSaveJson,
                        m_btnOpenLastReport, m_btnOpenLastReportFolder,
                        m_edHelp, m_btnOpenCmd, m_btnOpenPs };
        for (HWND hh : all) {
            if (!hh) continue;
            bool show = false;
            for (HWND v : visible) if (v == hh) show = true;
            ShowWindow(hh, show ? SW_SHOW : SW_HIDE);
        }
    };

    const int btnH = 28, margin = 8;

    switch (index) {
        case TAB_DASHBOARD: {
            hideAllExcept({ m_btnFullScan, m_edDiagnosis, m_lvDashboard });
            SetWindowPos(m_btnFullScan, nullptr, x + margin, y + margin, 200, btnH, SWP_NOZORDER);
            int diagH = 130;
            SetWindowPos(m_edDiagnosis, nullptr, x + margin, y + margin * 2 + btnH, w - margin * 2, diagH, SWP_NOZORDER);
            SetWindowPos(m_lvDashboard, nullptr, x + margin, y + margin * 3 + btnH + diagH,
                         w - margin * 2, h - (margin * 3 + btnH + diagH) - margin, SWP_NOZORDER);
            break;
        }
        case TAB_NETWORK: {
            hideAllExcept({ m_btnNetScan, m_btnTracert, m_lvNetwork });
            SetWindowPos(m_btnNetScan, nullptr, x + margin, y + margin, 200, btnH, SWP_NOZORDER);
            SetWindowPos(m_btnTracert, nullptr, x + margin + 208, y + margin, 220, btnH, SWP_NOZORDER);
            SetWindowPos(m_lvNetwork, nullptr, x + margin, y + margin * 2 + btnH,
                         w - margin * 2, h - (margin * 2 + btnH) - margin, SWP_NOZORDER);
            break;
        }
        case TAB_SYSTEM: {
            hideAllExcept({ m_btnSysScan, m_btnSfc, m_btnDismCheck, m_btnDismScan, m_lvSystem });
            int bx = x + margin;
            SetWindowPos(m_btnSysScan, nullptr, bx, y + margin, 170, btnH, SWP_NOZORDER); bx += 176;
            SetWindowPos(m_btnSfc, nullptr, bx, y + margin, 170, btnH, SWP_NOZORDER); bx += 176;
            SetWindowPos(m_btnDismCheck, nullptr, bx, y + margin, 150, btnH, SWP_NOZORDER); bx += 156;
            SetWindowPos(m_btnDismScan, nullptr, bx, y + margin, 180, btnH, SWP_NOZORDER);
            SetWindowPos(m_lvSystem, nullptr, x + margin, y + margin * 2 + btnH,
                         w - margin * 2, h - (margin * 2 + btnH) - margin, SWP_NOZORDER);
            break;
        }
        case TAB_HARDWARE: {
            hideAllExcept({ m_btnHwScan, m_lvHardware });
            SetWindowPos(m_btnHwScan, nullptr, x + margin, y + margin, 200, btnH, SWP_NOZORDER);
            SetWindowPos(m_lvHardware, nullptr, x + margin, y + margin * 2 + btnH,
                         w - margin * 2, h - (margin * 2 + btnH) - margin, SWP_NOZORDER);
            break;
        }
        case TAB_SECURITY: {
            hideAllExcept({ m_btnSecScan, m_lvSecurity });
            SetWindowPos(m_btnSecScan, nullptr, x + margin, y + margin, 200, btnH, SWP_NOZORDER);
            SetWindowPos(m_lvSecurity, nullptr, x + margin, y + margin * 2 + btnH,
                         w - margin * 2, h - (margin * 2 + btnH) - margin, SWP_NOZORDER);
            break;
        }
        case TAB_TOPOLOGY: {
            hideAllExcept({ m_topologyView });
            SetWindowPos(m_topologyView, nullptr, x + margin, y + margin,
                         w - margin * 2, h - margin * 2, SWP_NOZORDER);
            break;
        }
        case TAB_REPAIR: {
            hideAllExcept({ m_lvRepairCatalog, m_btnRunRepair, m_edRepairLog });
            int listH = (h - margin * 3 - btnH) * 3 / 5;
            SetWindowPos(m_lvRepairCatalog, nullptr, x + margin, y + margin, w - margin * 2, listH, SWP_NOZORDER);
            SetWindowPos(m_btnRunRepair, nullptr, x + margin, y + margin * 2 + listH, 200, btnH, SWP_NOZORDER);
            SetWindowPos(m_edRepairLog, nullptr, x + margin, y + margin * 3 + listH + btnH,
                         w - margin * 2, h - (margin * 3 + listH + btnH) - margin, SWP_NOZORDER);
            break;
        }
        case TAB_REPORT: {
            hideAllExcept({ m_edReport, m_btnSaveTxt, m_btnSaveHtml, m_btnSaveMd, m_btnSaveJson,
                             m_btnOpenLastReport, m_btnOpenLastReportFolder });
            int bx = x + margin;
            const int btnW = 130, gap = 8;
            SetWindowPos(m_btnSaveTxt, nullptr, bx, y + margin, btnW, btnH, SWP_NOZORDER); bx += btnW + gap;
            SetWindowPos(m_btnSaveHtml, nullptr, bx, y + margin, btnW, btnH, SWP_NOZORDER); bx += btnW + gap;
            SetWindowPos(m_btnSaveMd, nullptr, bx, y + margin, btnW, btnH, SWP_NOZORDER); bx += btnW + gap;
            SetWindowPos(m_btnSaveJson, nullptr, bx, y + margin, btnW, btnH, SWP_NOZORDER);

            int bx2 = x + margin;
            SetWindowPos(m_btnOpenLastReport, nullptr, bx2, y + margin * 2 + btnH, 190, btnH, SWP_NOZORDER);
            bx2 += 198;
            SetWindowPos(m_btnOpenLastReportFolder, nullptr, bx2, y + margin * 2 + btnH, 180, btnH, SWP_NOZORDER);

            SetWindowPos(m_edReport, nullptr, x + margin, y + margin * 3 + btnH * 2,
                         w - margin * 2, h - (margin * 3 + btnH * 2) - margin, SWP_NOZORDER);
            break;
        }
        case TAB_HELP: {
            hideAllExcept({ m_edHelp, m_btnOpenCmd, m_btnOpenPs });
            SetWindowPos(m_btnOpenCmd, nullptr, x + margin, y + margin, 220, btnH, SWP_NOZORDER);
            SetWindowPos(m_btnOpenPs, nullptr, x + margin + 228, y + margin, 200, btnH, SWP_NOZORDER);
            SetWindowPos(m_edHelp, nullptr, x + margin, y + margin * 2 + btnH,
                         w - margin * 2, h - (margin * 2 + btnH) - margin, SWP_NOZORDER);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Notifications / commands
// ---------------------------------------------------------------------------
void MainWindow::OnNotify(LPARAM lparam) {
    auto hdr = reinterpret_cast<NMHDR*>(lparam);
    if (hdr->hwndFrom == m_hTab && hdr->code == TCN_SELCHANGE) {
        OnTabChanged();
        return;
    }

    // Ctrl+C while a list view has focus copies the current selection - the
    // same result as the context menu's "Copy Selected Row(s)", just via the
    // keyboard shortcut people reach for automatically.
    if (hdr->code == LVN_KEYDOWN) {
        auto kd = reinterpret_cast<NMLVKEYDOWN*>(lparam);
        bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (kd->wVKey == 'C' && ctrlDown) {
            if (hdr->hwndFrom == m_lvRepairCatalog) {
                std::wstring text;
                int count = ListView_GetItemCount(m_lvRepairCatalog);
                for (int i = 0; i < count; ++i) {
                    if (ListView_GetItemState(m_lvRepairCatalog, i, LVIS_SELECTED) & LVIS_SELECTED) {
                        text += m_repairCatalog[i].name + L"\t" + m_repairCatalog[i].description + L"\t" +
                                (m_repairCatalog[i].requiresReboot ? L"Yes" : L"No") + L"\r\n";
                    }
                }
                if (!text.empty()) CopyTextToClipboard(m_hwnd, text);
            } else {
                auto backing = BackingResultsFor(hdr->hwndFrom);
                if (backing) CopySelectedRows(hdr->hwndFrom, *backing);
            }
        }
    }
}

void MainWindow::OnTabChanged() {
    int idx = TabCtrl_GetCurSel(m_hTab);
    LayoutTab(idx);

    if (idx == TAB_TOPOLOGY) {
        RefreshTopology();
        SetTimer(m_hwnd, ID_TOPOLOGY_TIMER, 3000, nullptr);
    } else {
        KillTimer(m_hwnd, ID_TOPOLOGY_TIMER);
    }
}

void MainWindow::RefreshTopology() {
    // GetAdapterTopology() only enumerates adapters/routes/neighbor-table
    // state (no ICMP pings, no blocking calls) - fast enough to call directly
    // on the UI thread from a 3-second timer without a worker thread.
    auto adapters = m_engine.GetAdapterTopology();
    TopologyView_SetData(m_topologyView, adapters);
}

void MainWindow::OnCommand(int id, HWND) {
    switch (id) {
        case ID_BTN_FULLSCAN: StartFullScan(); break;
        case ID_BTN_NETSCAN: StartNetworkScan(); break;
        case ID_BTN_TRACERT: StartTraceroute(); break;
        case ID_BTN_SYSSCAN: StartSystemScan(); break;
        case ID_BTN_SFC: StartSfcScan(); break;
        case ID_BTN_DISM_CHECK: StartDismCheck(); break;
        case ID_BTN_DISM_SCAN: StartDismScan(); break;
        case ID_BTN_HWSCAN: StartHardwareScan(); break;
        case ID_BTN_SECSCAN: StartSecurityScan(); break;
        case ID_BTN_SAVE_TXT: SaveReport(0); break;
        case ID_BTN_SAVE_HTML: SaveReport(1); break;
        case ID_BTN_SAVE_MD: SaveReport(2); break;
        case ID_BTN_SAVE_JSON: SaveReport(3); break;
        case ID_BTN_OPEN_LAST_REPORT: OpenLastSavedReport(); break;
        case ID_BTN_OPEN_LAST_REPORT_FOLDER: OpenLastSavedReportFolder(); break;
        case ID_BTN_OPEN_CMD:
            // Launched from an already-elevated process, so this inherits
            // elevation automatically - no separate "Run as administrator"
            // prompt needed.
            if (!LaunchExternalTool(L"cmd.exe")) {
                MessageBoxW(m_hwnd, L"Could not launch Command Prompt.", L"WinDiagPro", MB_OK | MB_ICONWARNING);
            }
            break;
        case ID_BTN_OPEN_PS:
            if (!LaunchExternalTool(L"powershell.exe")) {
                MessageBoxW(m_hwnd, L"Could not launch PowerShell.", L"WinDiagPro", MB_OK | MB_ICONWARNING);
            }
            break;
        case ID_BTN_RUNREPAIR: {
            if (m_busy) { MessageBeep(MB_ICONWARNING); break; }
            int count = ListView_GetItemCount(m_lvRepairCatalog);
            std::vector<std::wstring> toRun;
            for (int i = 0; i < count; ++i) {
                if (ListView_GetCheckState(m_lvRepairCatalog, i)) {
                    toRun.push_back(m_repairCatalog[i].id);
                }
            }
            if (toRun.empty()) {
                MessageBoxW(m_hwnd, L"Check one or more repair actions first.", L"WinDiagPro", MB_OK | MB_ICONINFORMATION);
                break;
            }
            if (!IsElevated()) {
                MessageBoxW(m_hwnd,
                    L"WinDiagPro is not running as Administrator. Most repair actions will fail.\r\n"
                    L"Close this app and re-launch it with \"Run as administrator\".",
                    L"Administrator privileges required", MB_OK | MB_ICONWARNING);
            }
            if (m_worker.joinable()) m_worker.join();
            SetBusy(true, L"Running repairs...");
            m_worker = std::thread([this, toRun]() {
                std::wstring combinedLog;
                for (auto& id : toRun) {
                    for (auto& action : m_repairCatalog) {
                        if (action.id == id) {
                            std::wstring log;
                            bool ok = action.execute(log);
                            combinedLog += L"=== " + action.name + L" : " + (ok ? L"OK" : L"FAILED") + L" ===\r\n";
                            combinedLog += log + L"\r\n\r\n";
                            break;
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(m_resultsMutex);
                    m_pendingRepairLog = combinedLog;
                }
                PostMessageW(m_hwnd, WM_APP_REPAIRLOG, 0, 0);
            });
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Copy-to-clipboard / right-click context menu
// ---------------------------------------------------------------------------
std::vector<CheckResult>* MainWindow::BackingResultsFor(HWND hwndLv) {
    if (hwndLv == m_lvDashboard) return &m_dashDisplayed;
    if (hwndLv == m_lvNetwork) return &m_netDisplayed;
    if (hwndLv == m_lvSystem) return &m_sysDisplayed;
    if (hwndLv == m_lvHardware) return &m_hwDisplayed;
    if (hwndLv == m_lvSecurity) return &m_secDisplayed;
    return nullptr;
}

void MainWindow::CopySelectedRows(HWND lv, const std::vector<CheckResult>& backing) {
    std::wstring text;
    int count = ListView_GetItemCount(lv);
    for (int i = 0; i < count && i < (int)backing.size(); ++i) {
        if (ListView_GetItemState(lv, i, LVIS_SELECTED) & LVIS_SELECTED) {
            text += ReportGenerator::FormatResultLineTsv(backing[i]) + L"\r\n";
        }
    }
    if (text.empty()) { MessageBeep(MB_ICONWARNING); return; }
    CopyTextToClipboard(m_hwnd, text);
}

void MainWindow::CopyAllRows(const std::vector<CheckResult>& backing) {
    std::wstring text;
    for (auto& r : backing) text += ReportGenerator::FormatResultLineTsv(r) + L"\r\n";
    if (text.empty()) { MessageBeep(MB_ICONWARNING); return; }
    CopyTextToClipboard(m_hwnd, text);
}

void MainWindow::ShowListContextMenu(HWND lv, POINT screenPt) {
    // The repair catalog isn't backed by CheckResult, so it gets a simpler
    // copy-only menu (its rows are already fully actionable via the checkbox
    // + "Run Checked Repairs" workflow).
    if (lv == m_lvRepairCatalog) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, CTX_COPY_SELECTED, L"Copy Selected Row(s)");
        AppendMenuW(menu, MF_STRING, CTX_COPY_ALL, L"Copy All Rows");
        SetForegroundWindow(m_hwnd); // ensures the popup dismisses correctly on click-away
        UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, m_hwnd, nullptr);
        DestroyMenu(menu);

        auto rowText = [this](int i) {
            return m_repairCatalog[i].name + L"\t" + m_repairCatalog[i].description + L"\t" +
                   (m_repairCatalog[i].requiresReboot ? L"Yes" : L"No");
        };
        if (cmd == CTX_COPY_SELECTED) {
            std::wstring text;
            int count = ListView_GetItemCount(lv);
            for (int i = 0; i < count; ++i) {
                if (ListView_GetItemState(lv, i, LVIS_SELECTED) & LVIS_SELECTED) text += rowText(i) + L"\r\n";
            }
            if (!text.empty()) CopyTextToClipboard(m_hwnd, text); else MessageBeep(MB_ICONWARNING);
        } else if (cmd == CTX_COPY_ALL) {
            std::wstring text;
            for (size_t i = 0; i < m_repairCatalog.size(); ++i) text += rowText((int)i) + L"\r\n";
            if (!text.empty()) CopyTextToClipboard(m_hwnd, text);
        }
        return;
    }

    auto backing = BackingResultsFor(lv);
    if (!backing) return;

    POINT client = screenPt;
    ScreenToClient(lv, &client);
    LVHITTESTINFO hit{};
    hit.pt = client;
    int hitIndex = ListView_HitTest(lv, &hit);

    // Right-clicking a row that isn't part of the current selection replaces
    // the selection with just that row (matches Explorer's behavior).
    if (hitIndex >= 0) {
        bool alreadySelected = (ListView_GetItemState(lv, hitIndex, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        if (!alreadySelected) {
            int count = ListView_GetItemCount(lv);
            for (int i = 0; i < count; ++i) ListView_SetItemState(lv, i, 0, LVIS_SELECTED);
            ListView_SetItemState(lv, hitIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CTX_COPY_SELECTED, L"Copy Selected Row(s)");
    AppendMenuW(menu, MF_STRING, CTX_COPY_ALL, L"Copy All Rows");

    m_pendingQuickActions.clear();
    if (hitIndex >= 0 && hitIndex < (int)backing->size()) {
        auto actions = GetQuickActionsFor((*backing)[hitIndex]);
        if (!actions.empty()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            UINT id = CTX_QUICKACTION_BASE;
            for (auto& a : actions) {
                AppendMenuW(menu, MF_STRING, id, a.label.c_str());
                m_pendingQuickActions.push_back(a);
                ++id;
            }
        }
    }

    SetForegroundWindow(m_hwnd); // ensures the popup dismisses correctly on click-away
    UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == CTX_COPY_SELECTED) {
        CopySelectedRows(lv, *backing);
    } else if (cmd == CTX_COPY_ALL) {
        CopyAllRows(*backing);
    } else if (cmd >= CTX_QUICKACTION_BASE) {
        size_t idx = cmd - CTX_QUICKACTION_BASE;
        if (idx < m_pendingQuickActions.size()) RunQuickAction(m_pendingQuickActions[idx]);
    }
}

void MainWindow::RunQuickAction(const QuickAction& action) {
    if (action.kind == QuickActionKind::LaunchTool) {
        if (!LaunchExternalTool(action.payload, action.arg)) {
            std::wstring msg = L"Could not launch: " + action.payload;
            MessageBoxW(m_hwnd, msg.c_str(), L"WinDiagPro", MB_OK | MB_ICONWARNING);
        }
        return;
    }

    if (m_busy) { MessageBeep(MB_ICONWARNING); return; }
    if (!IsElevated()) {
        MessageBoxW(m_hwnd,
            L"WinDiagPro is not running as Administrator. This action will likely fail.\r\n"
            L"Close this app and re-launch it with \"Run as administrator\".",
            L"Administrator privileges required", MB_OK | MB_ICONWARNING);
    }

    if (m_worker.joinable()) m_worker.join();
    SetBusy(true, L"Running: " + action.label + L"...");

    QuickAction copy = action;
    m_worker = std::thread([this, copy]() {
        std::wstring log;
        bool ok = false;
        if (copy.kind == QuickActionKind::RestartService) {
            ok = RepairEngine::RestartService(copy.payload, log);
        } else if (copy.kind == QuickActionKind::ReleaseRenewAdapter) {
            ok = RepairEngine::ReleaseRenewAdapter(copy.payload, log);
        } else if (copy.kind == QuickActionKind::RepairCatalogId) {
            for (auto& a : m_repairCatalog) {
                if (a.id == copy.payload) { ok = a.execute(log); break; }
            }
        }
        std::wstring combined = L"=== " + copy.label + L" : " + (ok ? L"OK" : L"FAILED") + L" ===\r\n" + log + L"\r\n\r\n";
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_pendingRepairLog = combined;
        }
        PostMessageW(m_hwnd, WM_APP_REPAIRLOG, 0, 0);
    });
}

// ---------------------------------------------------------------------------
// Scan dispatch (always on a background thread)
// ---------------------------------------------------------------------------
void MainWindow::SetBusy(bool busy, const std::wstring& statusText) {
    m_busy = busy;
    SendMessageW(m_hStatus, SB_SETTEXTW, 0, (LPARAM)statusText.c_str());
    if (m_hProgress) {
        if (busy) {
            SendMessageW(m_hProgress, PBM_SETMARQUEE, TRUE, 30);
            ShowWindow(m_hProgress, SW_SHOW);
        } else {
            SendMessageW(m_hProgress, PBM_SETMARQUEE, FALSE, 0);
            ShowWindow(m_hProgress, SW_HIDE);
        }
    }
    EnableWindow(m_btnFullScan, !busy);
    EnableWindow(m_btnNetScan, !busy);
    EnableWindow(m_btnSysScan, !busy);
    EnableWindow(m_btnSfc, !busy);
    EnableWindow(m_btnDismCheck, !busy);
    EnableWindow(m_btnDismScan, !busy);
    EnableWindow(m_btnHwScan, !busy);
    EnableWindow(m_btnSecScan, !busy);
    EnableWindow(m_btnRunRepair, !busy);
}

static void RebuildAll(std::vector<CheckResult>& all, const std::vector<CheckResult>& a,
                       const std::vector<CheckResult>& b, const std::vector<CheckResult>& c,
                       const std::vector<CheckResult>& d) {
    all.clear();
    all.insert(all.end(), a.begin(), a.end());
    all.insert(all.end(), b.begin(), b.end());
    all.insert(all.end(), c.begin(), c.end());
    all.insert(all.end(), d.begin(), d.end());
}

void MainWindow::StartFullScan() {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    SetBusy(true, L"Running full diagnostic...");
    m_worker = std::thread([this]() {
        auto net = m_engine.RunNetworkChecks();
        auto sys = m_engine.RunSystemChecks();
        auto hw = m_engine.RunHardwareChecks();
        auto sec = m_engine.RunSecurityChecks();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_netResults = net; m_sysResults = sys; m_hwResults = hw; m_secResults = sec;
            RebuildAll(m_allResults, m_netResults, m_sysResults, m_hwResults, m_secResults);
            m_diagnoses = RulesEngine::Analyze(m_allResults);
        }
        PostMessageW(m_hwnd, WM_APP_RESULTS, 0, 0);
    });
}

void MainWindow::StartNetworkScan() {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    SetBusy(true, L"Running network diagnostic...");
    m_worker = std::thread([this]() {
        auto net = m_engine.RunNetworkChecks();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_netResults = net;
            RebuildAll(m_allResults, m_netResults, m_sysResults, m_hwResults, m_secResults);
            m_diagnoses = RulesEngine::Analyze(m_allResults);
        }
        PostMessageW(m_hwnd, WM_APP_RESULTS, 0, 0);
    });
}

void MainWindow::StartSystemScan() {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    SetBusy(true, L"Running system checks...");
    m_worker = std::thread([this]() {
        auto sys = m_engine.RunSystemChecks();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_sysResults = sys;
            RebuildAll(m_allResults, m_netResults, m_sysResults, m_hwResults, m_secResults);
            m_diagnoses = RulesEngine::Analyze(m_allResults);
        }
        PostMessageW(m_hwnd, WM_APP_RESULTS, 0, 0);
    });
}

void MainWindow::StartHardwareScan() {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    SetBusy(true, L"Running hardware checks...");
    m_worker = std::thread([this]() {
        auto hw = m_engine.RunHardwareChecks();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_hwResults = hw;
            RebuildAll(m_allResults, m_netResults, m_sysResults, m_hwResults, m_secResults);
            m_diagnoses = RulesEngine::Analyze(m_allResults);
        }
        PostMessageW(m_hwnd, WM_APP_RESULTS, 0, 0);
    });
}

void MainWindow::StartSecurityScan() {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    SetBusy(true, L"Running security checks...");
    m_worker = std::thread([this]() {
        auto sec = m_engine.RunSecurityChecks();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_secResults = sec;
            RebuildAll(m_allResults, m_netResults, m_sysResults, m_hwResults, m_secResults);
            m_diagnoses = RulesEngine::Analyze(m_allResults);
        }
        PostMessageW(m_hwnd, WM_APP_RESULTS, 0, 0);
    });
}

void MainWindow::StartTraceroute() {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    SetBusy(true, L"Tracing route to the internet - this can take up to a minute...");
    m_worker = std::thread([this]() {
        auto res = m_engine.RunTraceroute();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            for (auto& r : res) m_netResults.push_back(r);
            RebuildAll(m_allResults, m_netResults, m_sysResults, m_hwResults, m_secResults);
            m_diagnoses = RulesEngine::Analyze(m_allResults);
        }
        PostMessageW(m_hwnd, WM_APP_RESULTS, 0, 0);
    });
}


void MainWindow::StartSfcScan() {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    SetBusy(true, L"Running SFC /scannow - this can take several minutes...");
    m_worker = std::thread([this]() {
        auto res = m_engine.RunSFCScan();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            for (auto& r : res) m_sysResults.push_back(r);
            RebuildAll(m_allResults, m_netResults, m_sysResults, m_hwResults, m_secResults);
            m_diagnoses = RulesEngine::Analyze(m_allResults);
        }
        PostMessageW(m_hwnd, WM_APP_RESULTS, 0, 0);
    });
}

void MainWindow::StartDismCheck() {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    SetBusy(true, L"Running DISM CheckHealth...");
    m_worker = std::thread([this]() {
        auto res = m_engine.RunDISMCheckHealth();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            for (auto& r : res) m_sysResults.push_back(r);
            RebuildAll(m_allResults, m_netResults, m_sysResults, m_hwResults, m_secResults);
            m_diagnoses = RulesEngine::Analyze(m_allResults);
        }
        PostMessageW(m_hwnd, WM_APP_RESULTS, 0, 0);
    });
}

void MainWindow::StartDismScan() {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    SetBusy(true, L"Running DISM ScanHealth - this can take several minutes...");
    m_worker = std::thread([this]() {
        auto res = m_engine.RunDISMScanHealth();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            for (auto& r : res) m_sysResults.push_back(r);
            RebuildAll(m_allResults, m_netResults, m_sysResults, m_hwResults, m_secResults);
            m_diagnoses = RulesEngine::Analyze(m_allResults);
        }
        PostMessageW(m_hwnd, WM_APP_RESULTS, 0, 0);
    });
}

// ---------------------------------------------------------------------------
// UI refresh (always runs on the UI thread, triggered via posted message)
// ---------------------------------------------------------------------------
void MainWindow::PopulateListView(HWND lv, const std::vector<CheckResult>& results) {
    ListView_DeleteAllItems(lv);
    int i = 0;
    for (auto& r : results) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        std::wstring status = StatusToString(r.status);
        item.pszText = const_cast<LPWSTR>(status.c_str());
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, i, 1, const_cast<LPWSTR>(CategoryToString(r.category).c_str()));
        ListView_SetItemText(lv, i, 2, const_cast<LPWSTR>(SeverityToString(r.severity).c_str()));
        ListView_SetItemText(lv, i, 3, const_cast<LPWSTR>(r.name.c_str()));
        ListView_SetItemText(lv, i, 4, const_cast<LPWSTR>(r.details.c_str()));
        ListView_SetItemText(lv, i, 5, const_cast<LPWSTR>(r.recommendation.c_str()));
        ++i;
    }
}

void MainWindow::OnResultsReady() {
    std::vector<CheckResult> net, sys, hw, sec, all;
    std::vector<Diagnosis> diagnoses;
    {
        std::lock_guard<std::mutex> lock(m_resultsMutex);
        net = m_netResults; sys = m_sysResults; hw = m_hwResults; sec = m_secResults;
        all = m_allResults; diagnoses = m_diagnoses;
    }

    PopulateListView(m_lvNetwork, net);
    PopulateListView(m_lvSystem, sys);
    PopulateListView(m_lvHardware, hw);
    PopulateListView(m_lvSecurity, sec);
    m_netDisplayed = net;
    m_sysDisplayed = sys;
    m_hwDisplayed = hw;
    m_secDisplayed = sec;

    // Dashboard: show only Fail/Warning items across everything, most severe first.
    std::vector<CheckResult> issues;
    for (auto& r : all) if (r.status == DiagStatus::Fail || r.status == DiagStatus::Warning) issues.push_back(r);
    std::sort(issues.begin(), issues.end(), [](const CheckResult& a, const CheckResult& b) {
        return (int)a.severity > (int)b.severity;
    });
    PopulateListView(m_lvDashboard, issues);
    m_dashDisplayed = issues;

    std::wstring diagText;
    if (diagnoses.empty()) {
        diagText = L"No significant issues detected in the areas scanned so far.";
    } else {
        for (auto& d : diagnoses) {
            diagText += L"[" + SeverityToString(d.severity) + L"] " + d.title + L"\r\n" + d.explanation + L"\r\n\r\n";
        }
    }
    SetWindowTextW(m_edDiagnosis, diagText.c_str());

    auto report = ReportGenerator::BuildText(all, diagnoses);
    SetWindowTextW(m_edReport, report.c_str());

    int pass = 0, fail = 0, warn = 0;
    for (auto& r : all) {
        if (r.status == DiagStatus::Pass) pass++;
        else if (r.status == DiagStatus::Fail) fail++;
        else if (r.status == DiagStatus::Warning) warn++;
    }
    wchar_t statusMsg[256];
    swprintf_s(statusMsg, L"Last scan: %d pass, %d fail, %d warning. Right-click a row for copy/fix options.", pass, fail, warn);
    SetBusy(false, statusMsg);
}

void MainWindow::OnRepairLogReady() {
    std::wstring log;
    {
        std::lock_guard<std::mutex> lock(m_resultsMutex);
        log = m_pendingRepairLog;
        m_pendingRepairLog.clear();
    }
    AppendLog(log);
    SetBusy(false, L"Repair actions complete. See the log below.");
    // Re-run all checks so the dashboard reflects the effect of the repair.
    StartFullScan();
}

void MainWindow::AppendLog(const std::wstring& text) {
    int len = GetWindowTextLengthW(m_edRepairLog);
    SendMessageW(m_edRepairLog, EM_SETSEL, len, len);
    SendMessageW(m_edRepairLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

void MainWindow::SaveReport(int format) {
    std::vector<CheckResult> all;
    std::vector<Diagnosis> diagnoses;
    {
        std::lock_guard<std::mutex> lock(m_resultsMutex);
        all = m_allResults; diagnoses = m_diagnoses;
    }
    std::wstring path;
    switch (format) {
        case 1: path = ReportGenerator::SaveHtml(ReportGenerator::BuildHtml(all, diagnoses)); break;
        case 2: path = ReportGenerator::SaveMarkdown(ReportGenerator::BuildMarkdown(all, diagnoses)); break;
        case 3: path = ReportGenerator::SaveJson(ReportGenerator::BuildJson(all, diagnoses)); break;
        default: path = ReportGenerator::SaveText(ReportGenerator::BuildText(all, diagnoses)); break;
    }
    if (path.empty()) {
        MessageBoxW(m_hwnd, L"Failed to save the report.", L"WinDiagPro", MB_OK | MB_ICONERROR);
        return;
    }

    m_lastSavedReportPath = path;
    m_lastSavedReportFormat = format;

    std::wstring msg = L"Report saved to:\r\n" + path + L"\r\n\r\nOpen it now?";
    if (MessageBoxW(m_hwnd, msg.c_str(), L"WinDiagPro", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        OpenReportFile(path, format);
    }
}

void MainWindow::OpenReportFile(const std::wstring& path, int format) {
    // .html gets its normal file association (a browser, which is what it's
    // for). .txt/.md/.json are forced open in Notepad specifically: some
    // systems have no default handler for .md/.json at all (which would
    // otherwise prompt an unhelpful "how do you want to open this file?"
    // dialog), and plain Notepad is exactly what you want anyway if you're
    // about to copy/paste the content into an editor, browser, or AI tool.
    bool ok = (format == 1)
        ? LaunchExternalTool(path)
        : LaunchExternalTool(L"notepad.exe", L"\"" + path + L"\"");
    if (!ok) {
        MessageBoxW(m_hwnd, L"Could not open the report file.", L"WinDiagPro", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::OpenLastSavedReport() {
    if (m_lastSavedReportPath.empty()) {
        MessageBoxW(m_hwnd, L"No report has been saved yet this session. Use one of the Save buttons first.",
                    L"WinDiagPro", MB_OK | MB_ICONINFORMATION);
        return;
    }
    OpenReportFile(m_lastSavedReportPath, m_lastSavedReportFormat);
}

void MainWindow::OpenLastSavedReportFolder() {
    if (m_lastSavedReportPath.empty()) {
        MessageBoxW(m_hwnd, L"No report has been saved yet this session. Use one of the Save buttons first.",
                    L"WinDiagPro", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // /select, highlights the specific file in Explorer rather than just
    // opening its containing folder generically.
    LaunchExternalTool(L"explorer.exe", L"/select,\"" + m_lastSavedReportPath + L"\"");
}
