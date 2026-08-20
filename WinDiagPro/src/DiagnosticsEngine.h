// DiagnosticsEngine.h - performs all diagnostic checks.
// Everything here is LOCAL: adapter/registry/service/WMI/event-log queries and
// pings to the default gateway / configured DNS servers on the LAN. No check in
// this file requires or assumes internet access, matching the offline design goal.
#pragma once
#include "Common.h"
#include "WMIHelper.h"
#include <vector>
#include <string>

struct NetworkAdapterInfo {
    std::wstring name;
    std::wstring description;
    std::wstring macAddress;
    std::wstring ipAddress;
    std::wstring gateway;
    std::vector<std::wstring> dnsServers;
    bool dhcpEnabled = false;
    bool connected = false;
    DWORD ifIndex = 0;
};

class DiagnosticsEngine {
public:
    DiagnosticsEngine();
    ~DiagnosticsEngine();

    bool Initialize();   // WSAStartup + WMI init
    void Shutdown();

    // Fast checks, safe to run automatically on dashboard load / periodic refresh.
    std::vector<CheckResult> RunNetworkChecks();
    std::vector<CheckResult> RunSystemChecks();      // service status, disk space, recent errors
    std::vector<CheckResult> RunHardwareChecks();    // WMI: devices with problems
    std::vector<CheckResult> RunSecurityChecks();    // firewall profiles, defender service

    // Slow / invasive checks - only run when the user explicitly asks (Repair tab).
    std::vector<CheckResult> RunSFCScan();            // sfc /scannow
    std::vector<CheckResult> RunDISMCheckHealth();     // DISM /CheckHealth (fast)
    std::vector<CheckResult> RunDISMScanHealth();      // DISM /ScanHealth (slower)

    std::vector<NetworkAdapterInfo> GetAdapters() const { return m_adapters; }

private:
    std::vector<NetworkAdapterInfo> m_adapters;
    WMIHelper m_wmi;
    bool m_winsockReady = false;

    bool EnumerateAdapters();
    bool PingHost(const std::wstring& hostOrIp, DWORD timeoutMs = 1500);
    bool CanResolve(const std::wstring& hostname);
};
