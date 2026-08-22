#include "QuickActions.h"
#include <shellapi.h>
#include <map>

#pragma comment(lib, "shell32.lib")

bool LaunchExternalTool(const std::wstring& target, const std::wstring& args) {
    HINSTANCE r = ShellExecuteW(nullptr, L"open", target.c_str(),
                                 args.empty() ? nullptr : args.c_str(),
                                 nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

// Mirrors the friendly-name table in DiagnosticsEngine.cpp so a "Service: X"
// CheckResult can be mapped back to the short service name RepairEngine needs.
static const std::wstring& ServiceShortNameFor(const std::wstring& friendlyName) {
    static const std::map<std::wstring, std::wstring> table = {
        { L"DHCP Client", L"Dhcp" },
        { L"DNS Client", L"Dnscache" },
        { L"Network Location Awareness", L"NlaSvc" },
        { L"WLAN AutoConfig", L"WlanSvc" },
        { L"Base Filtering Engine", L"BFE" },
        { L"Windows Firewall", L"MpsSvc" },
        { L"Windows Management Instrumentation", L"Winmgmt" },
        { L"Windows Update", L"wuauserv" },
        { L"Windows Defender Antivirus", L"WinDefend" },
    };
    static const std::wstring empty;
    auto it = table.find(friendlyName);
    return it != table.end() ? it->second : empty;
}

std::vector<QuickAction> GetQuickActionsFor(const CheckResult& r) {
    std::vector<QuickAction> actions;
    if (r.status == DiagStatus::Pass) return actions;

    // --- Services ---
    if (r.name.rfind(L"Service: ", 0) == 0) {
        std::wstring friendlyName = r.name.substr(9);
        const std::wstring& shortName = ServiceShortNameFor(friendlyName);
        if (!shortName.empty()) {
            actions.push_back({ L"Restart " + friendlyName + L" service",
                                 QuickActionKind::RestartService, shortName, L"" });
        }
        if (friendlyName == L"Windows Update") {
            actions.push_back({ L"Full Windows Update reset (fixes stuck/broken updates)",
                                 QuickActionKind::RepairCatalogId, L"full_wu_reset", L"" });
            actions.push_back({ L"Open Windows Update settings",
                                 QuickActionKind::LaunchTool, L"ms-settings:windowsupdate", L"" });
        } else if (friendlyName == L"Windows Firewall") {
            actions.push_back({ L"Open Windows Firewall settings",
                                 QuickActionKind::LaunchTool, L"firewall.cpl", L"" });
        } else if (friendlyName == L"Windows Defender Antivirus") {
            actions.push_back({ L"Open Windows Security",
                                 QuickActionKind::LaunchTool, L"windowsdefender:", L"" });
        }
        return actions;
    }

    // --- Disk space ---
    if (r.name.find(L"free space") != std::wstring::npos) {
        actions.push_back({ L"Open Storage Settings (Storage Sense)",
                             QuickActionKind::LaunchTool, L"ms-settings:storagesense", L"" });
        actions.push_back({ L"Open Disk Cleanup",
                             QuickActionKind::LaunchTool, L"cleanmgr.exe", L"" });
        return actions;
    }

    // --- Adapters ---
    if (r.name.rfind(L"Adapter: ", 0) == 0) {
        std::wstring adapterName = r.name.substr(9);
        if (r.status == DiagStatus::Fail) {
            // Fail here means GatewayScopeMismatch or Isolated-without-an-IP -
            // both are precisely fixed by cycling this one adapter's DHCP
            // lease, not by a machine-wide network reset.
            actions.push_back({ L"Release & renew DHCP on this adapter",
                                 QuickActionKind::ReleaseRenewAdapter, adapterName, L"" });
        }
        actions.push_back({ L"Open Network Connections",
                             QuickActionKind::LaunchTool, L"ncpa.cpl", L"" });
        actions.push_back({ L"Open Network & Internet Settings",
                             QuickActionKind::LaunchTool, L"ms-settings:network", L"" });
        return actions;
    }

    // --- Gateway ---
    if (r.name.find(L"Gateway reachability") != std::wstring::npos) {
        actions.push_back({ L"Open Network Connections",
                             QuickActionKind::LaunchTool, L"ncpa.cpl", L"" });
        actions.push_back({ L"Restart Network Location Awareness",
                             QuickActionKind::RepairCatalogId, L"restart_nla_svc", L"" });
        return actions;
    }

    // --- DNS ---
    if (r.name.find(L"DNS server") != std::wstring::npos) {
        actions.push_back({ L"Flush DNS cache",
                             QuickActionKind::RepairCatalogId, L"flush_dns", L"" });
        actions.push_back({ L"Restart DNS Client service",
                             QuickActionKind::RepairCatalogId, L"restart_dns_svc", L"" });
        return actions;
    }

    // --- Winsock ---
    if (r.name == L"Winsock catalog") {
        actions.push_back({ L"Reset Winsock",
                             QuickActionKind::RepairCatalogId, L"reset_winsock", L"" });
        actions.push_back({ L"Reset TCP/IP stack",
                             QuickActionKind::RepairCatalogId, L"reset_tcpip", L"" });
        return actions;
    }

    // --- Event log ---
    if (r.name.find(L"Recent System log errors") != std::wstring::npos) {
        actions.push_back({ L"Open Event Viewer",
                             QuickActionKind::LaunchTool, L"eventvwr.msc", L"" });
        return actions;
    }

    // --- Problem devices ---
    if (r.name.rfind(L"Problem device: ", 0) == 0) {
        actions.push_back({ L"Open Device Manager",
                             QuickActionKind::LaunchTool, L"devmgmt.msc", L"" });
        return actions;
    }

    // --- Firewall profiles ---
    if (r.name.rfind(L"Firewall profile: ", 0) == 0) {
        actions.push_back({ L"Open Windows Firewall settings",
                             QuickActionKind::LaunchTool, L"firewall.cpl", L"" });
        return actions;
    }

    // --- Hosts file ---
    if (r.name == L"Hosts file") {
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring hostsPath = std::wstring(sysDir) + L"\\drivers\\etc\\hosts";
        actions.push_back({ L"Open hosts file in Notepad",
                             QuickActionKind::LaunchTool, L"notepad.exe", L"\"" + hostsPath + L"\"" });
        return actions;
    }

    // --- Proxy settings ---
    if (r.name == L"Browser/app proxy (WinINet)") {
        actions.push_back({ L"Open proxy settings",
                             QuickActionKind::LaunchTool, L"ms-settings:network-proxy", L"" });
        return actions;
    }
    if (r.name == L"System proxy (WinHTTP)") {
        actions.push_back({ L"Reset WinHTTP proxy",
                             QuickActionKind::RepairCatalogId, L"reset_winhttp_proxy", L"" });
        actions.push_back({ L"Open proxy settings",
                             QuickActionKind::LaunchTool, L"ms-settings:network-proxy", L"" });
        return actions;
    }

    // --- Clock / time sync ---
    if (r.name == L"System clock / time sync") {
        actions.push_back({ L"Open Date & Time settings",
                             QuickActionKind::LaunchTool, L"ms-settings:dateandtime", L"" });
        actions.push_back({ L"Restart Windows Time service",
                             QuickActionKind::RestartService, L"W32Time", L"" });
        return actions;
    }

    // --- System Restore ---
    if (r.name == L"System Restore points") {
        actions.push_back({ L"Create Restore Point Now",
                             QuickActionKind::RepairCatalogId, L"create_restore_point", L"" });
        actions.push_back({ L"Open System Restore",
                             QuickActionKind::LaunchTool, L"rstrui.exe", L"" });
        return actions;
    }

    // --- Autostart ---
    if (r.name == L"Autostart programs") {
        actions.push_back({ L"Open Task Manager (Startup tab)",
                             QuickActionKind::LaunchTool, L"taskmgr.exe", L"" });
        return actions;
    }

    return actions;
}
