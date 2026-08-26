// main.cpp - entry point. Launches the GUI by default. Also supports a
// headless CLI mode (useful when running from WinRE / a recovery USB stick,
// or for scripting) via command-line switches:
//
//   WinDiagPro.exe /cli /full            Run all checks, print + save a report
//   WinDiagPro.exe /cli /network         Network checks only
//   WinDiagPro.exe /cli /system          System checks only
//   WinDiagPro.exe /cli /report:<path>   Save the report to a specific folder
//   WinDiagPro.exe /cli /html            Save as .html instead of .txt
//   WinDiagPro.exe /cli /md              Save as .md instead of .txt
//   WinDiagPro.exe /cli /json            Save as .json instead of .txt
//
// All of this is local-only: no argument here causes any network traffic
// beyond the LAN-local ping/DNS checks described in DiagnosticsEngine.h.
#include "Common.h"
#include "DiagnosticsEngine.h"
#include "RulesEngine.h"
#include "ReportGenerator.h"
#include "MainWindow.h"
#include <iostream>
#include <string>
#include <vector>
#include <shellapi.h>

// Common Controls v6 (themed ListView/TabControl/buttons) requires an
// assembly dependency in the executable's manifest. Rather than ship a
// separate app.manifest that has to be merged in by mt.exe (that merge step
// is a known source of flaky LNK1327/CVT1100 failures depending on the
// machine/AV setup), synthesize it directly via a linker pragma: the linker
// generates the manifest itself (no external file, no merge tool involved).
// UAC elevation is likewise requested via the UACExecutionLevel project
// property (see WinDiagPro.vcxproj) rather than manifest XML.
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' " \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static bool HasSwitch(const std::vector<std::wstring>& args, const wchar_t* name) {
    for (auto& a : args) if (a == name) return true;
    return false;
}

static std::wstring GetSwitchValue(const std::vector<std::wstring>& args, const wchar_t* prefix) {
    for (auto& a : args) {
        if (a.rfind(prefix, 0) == 0) return a.substr(wcslen(prefix));
    }
    return L"";
}

static int RunCli(const std::vector<std::wstring>& args) {
    // Give ourselves a console since GUI subsystem apps don't have one by default.
    AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);

    std::wcout << L"WinDiagPro CLI - offline diagnostic mode\r\n";
    if (!IsElevated()) {
        std::wcout << L"WARNING: not running as Administrator - some checks/repairs will be skipped.\r\n";
    }

    DiagnosticsEngine engine;
    engine.Initialize();

    std::vector<CheckResult> all;
    bool doNetwork = HasSwitch(args, L"/network") || HasSwitch(args, L"/full") || args.size() <= 1;
    bool doSystem  = HasSwitch(args, L"/system")  || HasSwitch(args, L"/full");
    bool doHw      = HasSwitch(args, L"/hardware")|| HasSwitch(args, L"/full");
    bool doSec     = HasSwitch(args, L"/security")|| HasSwitch(args, L"/full");

    if (doNetwork) { auto r = engine.RunNetworkChecks(); all.insert(all.end(), r.begin(), r.end()); }
    if (doSystem)  { auto r = engine.RunSystemChecks();  all.insert(all.end(), r.begin(), r.end()); }
    if (doHw)      { auto r = engine.RunHardwareChecks();all.insert(all.end(), r.begin(), r.end()); }
    if (doSec)     { auto r = engine.RunSecurityChecks();all.insert(all.end(), r.begin(), r.end()); }

    auto diagnoses = RulesEngine::Analyze(all);
    auto text = ReportGenerator::BuildText(all, diagnoses);
    std::wcout << text;

    std::wstring folder = GetSwitchValue(args, L"/report:");
    std::wstring saved;
    if (HasSwitch(args, L"/html")) {
        saved = ReportGenerator::SaveHtml(ReportGenerator::BuildHtml(all, diagnoses), folder);
    } else if (HasSwitch(args, L"/md")) {
        saved = ReportGenerator::SaveMarkdown(ReportGenerator::BuildMarkdown(all, diagnoses), folder);
    } else if (HasSwitch(args, L"/json")) {
        saved = ReportGenerator::SaveJson(ReportGenerator::BuildJson(all, diagnoses), folder);
    } else {
        saved = ReportGenerator::SaveText(text, folder);
    }
    if (!saved.empty()) {
        std::wcout << L"\r\nReport saved to: " << saved << L"\r\n";
    }

    engine.Shutdown();

    std::wcout << L"\r\nPress Enter to exit...";
    std::wcin.get();
    FreeConsole();
    return 0;
}

// Sets per-monitor-v2 DPI awareness if the running OS supports it (Windows 10
// 1703+ / all of Windows 11). Loaded dynamically via GetProcAddress rather
// than a static import so this never becomes a hard link-time or load-time
// dependency on older systems - it just silently no-ops there instead.
static void EnablePerMonitorDpiAwareness() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) return;
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto setCtx = reinterpret_cast<SetCtxFn>(GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
    if (setCtx) {
        setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    EnablePerMonitorDpiAwareness();

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (argvW) {
        for (int i = 0; i < argc; ++i) args.push_back(argvW[i]);
        LocalFree(argvW);
    }

    if (HasSwitch(args, L"/cli")) {
        return RunCli(args);
    }

    return RunGui(hInstance, nCmdShow);
}
