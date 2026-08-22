#include "RepairEngine.h"
#include "WMIHelper.h"
#include <shlobj.h>
#include <set>

#pragma comment(lib, "shell32.lib")

// --- internal helpers used only by FullWindowsUpdateReset (stop and start
// need to happen as separate steps, with folder renames in between, unlike
// the simple stop-then-immediately-start RestartService() below) ---
namespace {

bool StopServiceOnly(const std::wstring& name, std::wstring& log) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { log += name + L": could not open Service Control Manager\r\n"; return false; }

    SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) { log += name + L": service not found (may not exist on this system)\r\n"; CloseServiceHandle(scm); return false; }

    SERVICE_STATUS status{};
    QueryServiceStatus(svc, &status);
    if (status.dwCurrentState != SERVICE_STOPPED) {
        ControlService(svc, SERVICE_CONTROL_STOP, &status);
        for (int i = 0; i < 30 && status.dwCurrentState != SERVICE_STOPPED; ++i) {
            Sleep(500);
            QueryServiceStatus(svc, &status);
        }
    }
    bool stopped = status.dwCurrentState == SERVICE_STOPPED;
    log += name + (stopped ? L": stopped\r\n" : L": failed to stop\r\n");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return stopped;
}

bool StartServiceOnly(const std::wstring& name, std::wstring& log) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { log += name + L": could not open Service Control Manager\r\n"; return false; }

    SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) { log += name + L": service not found\r\n"; CloseServiceHandle(scm); return false; }

    BOOL started = StartServiceW(svc, 0, nullptr);
    SERVICE_STATUS status{};
    for (int i = 0; i < 30; ++i) {
        QueryServiceStatus(svc, &status);
        if (status.dwCurrentState == SERVICE_RUNNING) break;
        Sleep(500);
    }
    bool running = status.dwCurrentState == SERVICE_RUNNING;
    log += name + (running ? L": running\r\n" : L": did not reach Running state\r\n");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return running || started;
}

void RenameOutOfTheWay(const std::wstring& from, std::wstring& log) {
    if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log += from + L": not present, skipping\r\n";
        return;
    }
    std::wstring to = from + L".bak_" + CurrentTimestampForFilename();
    if (MoveFileExW(from.c_str(), to.c_str(), 0)) {
        log += L"Renamed " + from + L" -> " + to + L"\r\n";
    } else {
        log += L"Could not rename " + from + L" (error " + std::to_wstring(GetLastError()) +
               L" - it may be in use; a reboot then re-running this repair usually clears that)\r\n";
    }
}

} // namespace

bool RepairEngine::FullWindowsUpdateReset(std::wstring& log) {
    log.clear();
    log += L"Full Windows Update component reset starting.\r\n";
    log += L"Nothing is deleted - old cache folders are renamed with a timestamped "
           L".bak suffix, so this is fully reversible. Windows rebuilds them "
           L"automatically the next time it checks for updates.\r\n\r\n";

    const wchar_t* services[] = { L"wuauserv", L"BITS", L"CryptSvc", L"msiserver" };

    log += L"-- Stopping services --\r\n";
    bool allStopped = true;
    for (auto s : services) allStopped &= StopServiceOnly(s, log);

    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::wstring softwareDistribution = std::wstring(windir) + L"\\SoftwareDistribution";
    std::wstring catroot2 = std::wstring(windir) + L"\\System32\\catroot2";

    log += L"\r\n-- Resetting cache folders --\r\n";
    RenameOutOfTheWay(softwareDistribution, log);
    RenameOutOfTheWay(catroot2, log);

    log += L"\r\n-- Restarting services --\r\n";
    bool allStarted = true;
    for (auto s : services) allStarted &= StartServiceOnly(s, log);

    log += L"\r\nDone. Try Windows Update again; if it still fails, running DISM "
           L"RestoreHealth followed by SFC (System tab) addresses most remaining cases.\r\n";

    return allStopped && allStarted;
}

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

bool RepairEngine::ReleaseRenewAdapter(const std::wstring& adapterName, std::wstring& log) {
    std::wstring release = RunCommandCaptureOutput(L"ipconfig /release \"" + adapterName + L"\"", 20000);
    std::wstring renew = RunCommandCaptureOutput(L"ipconfig /renew \"" + adapterName + L"\"", 30000);
    log = release + L"\r\n" + renew;
    return renew.find(L"IPv4 Address") != std::wstring::npos ||
           renew.find(L"IP Address") != std::wstring::npos;
}

bool RepairEngine::BackupNetworkDrivers(std::wstring& log, std::wstring& outFolder) {
    log.clear();
    outFolder.clear();

    WMIHelper wmi;
    if (!wmi.Initialize()) {
        log = L"Could not connect to WMI - driver backup unavailable.";
        return false;
    }

    auto rows = wmi.Query(L"SELECT DeviceName, InfName FROM Win32_PnPSignedDriver WHERE DeviceClass='Net'");
    wmi.Shutdown();

    if (rows.empty()) {
        log = L"No network device drivers were found via WMI.";
        return false;
    }

    wchar_t docsPath[MAX_PATH];
    std::wstring baseDir = SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docsPath))
                                ? docsPath : L".";
    std::wstring folder = baseDir + L"\\WinDiagPro_NetworkDriverBackup_" + CurrentTimestampForFilename();
    CreateDirectoryW(folder.c_str(), nullptr);

    std::set<std::wstring> seenInf;
    int exported = 0;
    for (auto& row : rows) {
        std::wstring deviceName = row.count(L"DeviceName") ? row[L"DeviceName"] : L"(unknown device)";
        std::wstring infName = row.count(L"InfName") ? row[L"InfName"] : L"";
        if (infName.empty() || seenInf.count(infName)) continue;
        seenInf.insert(infName);

        log += L"Exporting driver for " + deviceName + L" (" + infName + L")...\r\n";
        std::wstring cmd = L"pnputil /export-driver " + infName + L" \"" + folder + L"\"";
        std::wstring result = RunCommandCaptureOutput(cmd, 30000);
        log += result + L"\r\n";
        if (result.find(L"exported successfully") != std::wstring::npos ||
            result.find(L"Driver package exported") != std::wstring::npos ||
            result.find(L"Published Name") != std::wstring::npos) {
            ++exported;
        }
    }

    outFolder = folder;
    log += L"\r\n" + std::to_wstring(exported) + L" of " + std::to_wstring(seenInf.size()) +
           L" network driver package(s) exported to:\r\n" + folder;
    return exported > 0;
}

std::wstring RepairEngine::ExportNetworkConfigSnapshot(std::wstring& log, const std::wstring& folder) {
    log.clear();
    std::wstring dir = folder;
    if (dir.empty()) {
        wchar_t docsPath[MAX_PATH];
        dir = SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docsPath)) ? docsPath : L".";
    }
    std::wstring path = dir + L"\\WinDiagPro_NetworkSnapshot_" + CurrentTimestampForFilename() + L".txt";

    std::wstring content;
    content += L"WinDiagPro network configuration snapshot\r\n";
    content += L"Captured: " + CurrentTimestamp() + L"\r\n";
    content += L"(For before/after comparison - nothing here is changed by capturing it.)\r\n\r\n";

    content += L"==================== ipconfig /all ====================\r\n";
    content += RunCommandCaptureOutput(L"ipconfig /all", 15000) + L"\r\n\r\n";

    content += L"==================== route print ====================\r\n";
    content += RunCommandCaptureOutput(L"route print", 15000) + L"\r\n\r\n";

    content += L"==================== netsh interface ip show config ====================\r\n";
    content += RunCommandCaptureOutput(L"netsh interface ip show config", 15000) + L"\r\n";

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log = L"Could not create snapshot file at " + path;
        return L"";
    }
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(h, &bom, sizeof(bom), &written, nullptr);
    WriteFile(h, content.c_str(), (DWORD)(content.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);

    log = L"Network configuration snapshot saved to:\r\n" + path;
    return path;
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

    actions.push_back({
        L"full_wu_reset", L"Full Windows Update Reset",
        L"Stops Windows Update/BITS/Cryptographic/Installer services, renames the "
        L"SoftwareDistribution and catroot2 cache folders out of the way (nothing "
        L"is deleted - fully reversible), then restarts the services. Fixes a "
        L"Windows Update service that runs but never actually installs updates, "
        L"which a plain service restart usually can't.",
        false,
        [](std::wstring& log) { return RepairEngine::FullWindowsUpdateReset(log); }
    });

    actions.push_back({
        L"backup_net_drivers", L"Backup Network Drivers",
        L"Exports the driver package for every installed network adapter (already on "
        L"this machine's driver store - nothing is downloaded) to a folder in "
        L"Documents, using the built-in pnputil tool. Run this before any invasive "
        L"network repair as a safety net: if something ever goes wrong, you have an "
        L"offline copy ready to reinstall without needing internet access.",
        false,
        [](std::wstring& log) { std::wstring folder; return RepairEngine::BackupNetworkDrivers(log, folder); }
    });

    actions.push_back({
        L"backup_net_config", L"Backup Current Network Configuration",
        L"Saves a snapshot of ipconfig /all, route print, and netsh interface config "
        L"to a timestamped file in Documents. Doesn't change anything - it's purely "
        L"a before/after reference so you can compare what changed after a repair.",
        false,
        [](std::wstring& log) { auto path = RepairEngine::ExportNetworkConfigSnapshot(log); return !path.empty(); }
    });

    return actions;
}
