#include "RepairEngine.h"

bool RepairEngine::FlushDns(std::wstring& log) {
    log = RunCommandCaptureOutput(L"ipconfig /flushdns", 15000);
    return log.find(L"Successfully flushed") != std::wstring::npos ||
           log.find(L"successfully flushed") != std::wstring::npos;
}

bool RepairEngine::ResetWinsock(std::wstring& log) {
    log = RunCommandCaptureOutput(L"netsh winsock reset", 15000);
    return log.find(L"reset successfully") != std::wstring::npos ||
           log.find(L"Winsock Catalog") != std::wstring::npos;
}

bool RepairEngine::ResetTcpIp(std::wstring& log) {
    log = RunCommandCaptureOutput(L"netsh int ip reset", 15000);
    return log.find(L"Resetting") != std::wstring::npos || !log.empty();
}

bool RepairEngine::RenewDhcpAll(std::wstring& log) {
    std::wstring release = RunCommandCaptureOutput(L"ipconfig /release", 20000);
    std::wstring renew = RunCommandCaptureOutput(L"ipconfig /renew", 30000);
    log = release + L"\r\n" + renew;
    return renew.find(L"IPv4 Address") != std::wstring::npos ||
           renew.find(L"IP Address") != std::wstring::npos;
}

bool RepairEngine::RestartService(const std::wstring& serviceName, std::wstring& log) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { log = L"Could not open Service Control Manager (run as Administrator)."; return false; }

    SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) { log = L"Could not open service " + serviceName + L"."; CloseServiceHandle(scm); return false; }

    SERVICE_STATUS status{};
    QueryServiceStatus(svc, &status);
    if (status.dwCurrentState != SERVICE_STOPPED) {
        ControlService(svc, SERVICE_CONTROL_STOP, &status);
        for (int i = 0; i < 30 && status.dwCurrentState != SERVICE_STOPPED; ++i) {
            Sleep(500);
            QueryServiceStatus(svc, &status);
        }
    }

    BOOL started = StartServiceW(svc, 0, nullptr);
    for (int i = 0; i < 30 && QueryServiceStatus(svc, &status) && status.dwCurrentState != SERVICE_RUNNING; ++i) {
        Sleep(500);
    }

    log = serviceName + (status.dwCurrentState == SERVICE_RUNNING ? L" restarted successfully."
                                                                   : L" failed to reach the Running state.");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return status.dwCurrentState == SERVICE_RUNNING || started;
}

std::vector<RepairAction> RepairEngine::GetCatalog() {
    std::vector<RepairAction> actions;

    actions.push_back({
        L"flush_dns", L"Flush DNS Cache",
        L"Clears the local DNS resolver cache. Safe, no reboot required.",
        false,
        [](std::wstring& log) { return RepairEngine::FlushDns(log); }
    });

    actions.push_back({
        L"reset_winsock", L"Reset Winsock",
        L"Resets the Winsock protocol catalog to its default state. A restart is recommended afterwards.",
        true,
        [](std::wstring& log) { return RepairEngine::ResetWinsock(log); }
    });

    actions.push_back({
        L"reset_tcpip", L"Reset TCP/IP Stack",
        L"Resets the TCP/IP stack configuration to defaults. A restart is required afterwards.",
        true,
        [](std::wstring& log) { return RepairEngine::ResetTcpIp(log); }
    });

    actions.push_back({
        L"renew_dhcp", L"Renew DHCP Lease",
        L"Releases and renews the IP address lease on all adapters.",
        false,
        [](std::wstring& log) { return RepairEngine::RenewDhcpAll(log); }
    });

    actions.push_back({
        L"restart_dhcp_svc", L"Restart DHCP Client Service",
        L"Stops and starts the DHCP Client Windows service.",
        false,
        [](std::wstring& log) { return RepairEngine::RestartService(L"Dhcp", log); }
    });

    actions.push_back({
        L"restart_dns_svc", L"Restart DNS Client Service",
        L"Stops and starts the DNS Client (Dnscache) Windows service.",
        false,
        [](std::wstring& log) { return RepairEngine::RestartService(L"Dnscache", log); }
    });

    actions.push_back({
        L"restart_nla_svc", L"Restart Network Location Awareness",
        L"Stops and starts the Network Location Awareness service.",
        false,
        [](std::wstring& log) { return RepairEngine::RestartService(L"NlaSvc", log); }
    });

    actions.push_back({
        L"restart_firewall_svc", L"Restart Windows Firewall Service",
        L"Stops and starts the Windows Firewall (MpsSvc) service.",
        false,
        [](std::wstring& log) { return RepairEngine::RestartService(L"MpsSvc", log); }
    });

    return actions;
}
