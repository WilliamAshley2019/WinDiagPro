// MainWindow.h - native Win32 GUI shell (no WinUI3/XAML, no external UI framework).
// Chosen deliberately: it compiles with just the Windows SDK that ships with
// Visual Studio, needs no NuGet/Windows App SDK runtime, and packages as one
// small standalone EXE - ideal for an offline recovery/troubleshooting tool
// that may need to run from a USB stick on a machine with nothing else installed.
#pragma once
#include "Common.h"
#include "DiagnosticsEngine.h"
#include "RepairEngine.h"
#include "RulesEngine.h"
#include "QuickActions.h"
#include "TopologyView.h"
#include "HelpContent.h"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    int RunMessageLoop();

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void OnCreate(HWND hwnd);
    void OnSize(int width, int height);
    void OnCommand(int id, HWND ctrl);
    void OnNotify(LPARAM lparam);
    void OnTabChanged();
    void RefreshTopology();
    void OnResultsReady(); // marshaled from worker thread via WM_APP message
    void OnRepairLogReady();

    void LayoutTab(int index);
    void PopulateListView(HWND lv, const std::vector<CheckResult>& results);
    void AppendLog(const std::wstring& text);

    // Long-running work, always dispatched to a background thread.
    void StartFullScan();
    void StartNetworkScan();
    void StartSystemScan();
    void StartHardwareScan();
    void StartSecurityScan();
    void StartSfcScan();
    void StartDismCheck();
    void StartDismScan();
    void SaveReport(int format); // 0=txt 1=html 2=md 3=json

    void SetBusy(bool busy, const std::wstring& statusText);

    // --- Copy-to-clipboard / right-click context menu on diagnostic list views ---
    // Returns the CheckResult vector currently backing a given list view (or
    // nullptr if hwndLv isn't one of the diagnostic list views).
    std::vector<CheckResult>* BackingResultsFor(HWND hwndLv);
    void CopySelectedRows(HWND lv, const std::vector<CheckResult>& backing);
    void CopyAllRows(const std::vector<CheckResult>& backing);
    void ShowListContextMenu(HWND lv, POINT screenPt);
    void RunQuickAction(const QuickAction& action);

    HWND m_hwnd = nullptr;
    HWND m_hTab = nullptr;
    HWND m_hStatus = nullptr;
    HWND m_hProgress = nullptr;

    // Per-tab controls
    HWND m_lvDashboard = nullptr, m_edDiagnosis = nullptr;
    HWND m_lvNetwork = nullptr;
    HWND m_lvSystem = nullptr;
    HWND m_lvHardware = nullptr;
    HWND m_lvSecurity = nullptr;
    HWND m_topologyView = nullptr;
    HWND m_edHelp = nullptr;
    HWND m_btnOpenCmd = nullptr, m_btnOpenPs = nullptr;
    HWND m_lvRepairCatalog = nullptr, m_edRepairLog = nullptr, m_btnRunRepair = nullptr;
    HWND m_edReport = nullptr;
    HWND m_btnSaveTxt = nullptr, m_btnSaveHtml = nullptr, m_btnSaveMd = nullptr, m_btnSaveJson = nullptr;

    HWND m_btnFullScan = nullptr, m_btnNetScan = nullptr, m_btnSysScan = nullptr;
    HWND m_btnHwScan = nullptr, m_btnSecScan = nullptr;
    HWND m_btnSfc = nullptr, m_btnDismCheck = nullptr, m_btnDismScan = nullptr;

    DiagnosticsEngine m_engine;
    std::vector<RepairAction> m_repairCatalog;

    std::vector<CheckResult> m_netResults, m_sysResults, m_hwResults, m_secResults;
    std::vector<CheckResult> m_allResults; // concatenation of the four above, rebuilt after each scan
    std::vector<Diagnosis> m_diagnoses;
    std::mutex m_resultsMutex;
    std::wstring m_pendingRepairLog; // set by RunRepairAction before dispatching to worker thread

    // UI-thread-only copies of what's currently displayed in each list view,
    // in the same order as the rows - lets the right-click handler map a
    // clicked row index back to its full CheckResult (details/recommendation
    // aren't visible as columns wide enough to read in full).
    std::vector<CheckResult> m_dashDisplayed, m_netDisplayed, m_sysDisplayed, m_hwDisplayed, m_secDisplayed;

    // Pending quick-action context menu state (set by ShowListContextMenu,
    // read back in OnCommand when the user picks a menu item).
    std::vector<QuickAction> m_pendingQuickActions;

    std::thread m_worker;
    std::atomic<bool> m_busy{ false };

    HFONT m_uiFont = nullptr;
};

int RunGui(HINSTANCE hInstance, int nCmdShow);
