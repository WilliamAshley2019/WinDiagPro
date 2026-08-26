// QuickActions.h - maps a CheckResult to concrete, one-click fixes: either a
// RepairEngine action, or launching the actual Windows tool/settings page
// that addresses it (Disk Cleanup, Storage Settings, Device Manager, Windows
// Firewall, Event Viewer, Network Connections, etc). This is what turns a
// diagnosis like "low disk space" into something actionable instead of just
// a message telling the user to go do it themselves.
#pragma once
#include "Common.h"
#include <vector>
#include <string>
#include <functional>

enum class QuickActionKind {
    RestartService,   // payload = service short name (e.g. L"wuauserv")
    RepairCatalogId,  // payload = RepairEngine::GetCatalog() id (e.g. L"flush_dns")
    LaunchTool,        // payload = executable/URI, arg = optional arguments
    ReleaseRenewAdapter, // payload = adapter friendly name (e.g. L"Ethernet 2")
    SetPublicDns,      // payload = adapter friendly name - switch that adapter to 1.1.1.1/1.0.0.1
    RestoreDhcpDns,    // payload = adapter friendly name - undo SetPublicDns
};

struct QuickAction {
    std::wstring label;
    QuickActionKind kind;
    std::wstring payload;
    std::wstring arg; // only used for LaunchTool
};

// Returns 0+ contextual actions relevant to this specific result. Empty for
// checks that passed, or that don't have a known concrete remedy.
std::vector<QuickAction> GetQuickActionsFor(const CheckResult& r);

// Launches an external tool/URI (Control Panel applet, ms-settings: page,
// devmgmt.msc, cleanmgr.exe, etc.) via ShellExecute. Returns false if the
// shell could not launch it (e.g. the target doesn't exist on this system).
bool LaunchExternalTool(const std::wstring& target, const std::wstring& args = L"");
