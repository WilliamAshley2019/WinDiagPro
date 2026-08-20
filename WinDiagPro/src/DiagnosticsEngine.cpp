#include "DiagnosticsEngine.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2spi.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <winevt.h>
#include <netfw.h>
#include <comdef.h>
#include <memory>
#include <algorithm>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wevtapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

DiagnosticsEngine::DiagnosticsEngine() {}
DiagnosticsEngine::~DiagnosticsEngine() { Shutdown(); }

bool DiagnosticsEngine::Initialize() {
    WSADATA wsaData;
    m_winsockReady = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    m_wmi.Initialize(); // hardware checks degrade gracefully if this fails
    return m_winsockReady;
}

void DiagnosticsEngine::Shutdown() {
    m_wmi.Shutdown();
    if (m_winsockReady) {
        WSACleanup();
        m_winsockReady = false;
    }
}

// ---------------------------------------------------------------------------
// Adapter enumeration
// ---------------------------------------------------------------------------
bool DiagnosticsEngine::EnumerateAdapters() {
    m_adapters.clear();

    ULONG bufLen = 15000;
    std::unique_ptr<BYTE[]> buffer(new BYTE[bufLen]);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.get());

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    DWORD ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, pAddresses, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.reset(new BYTE[bufLen]);
        pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.get());
        ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, pAddresses, &bufLen);
    }
    if (ret != NO_ERROR) return false;

    for (PIP_ADAPTER_ADDRESSES p = pAddresses; p != nullptr; p = p->Next) {
        if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        // Skip adapters that are administratively disabled AND never had a UP status,
        // but keep media-disconnected physical adapters so the user can see "cable unplugged".
        if (p->IfType == IF_TYPE_TUNNEL) continue;

        NetworkAdapterInfo a;
        a.name = p->FriendlyName ? p->FriendlyName : L"(unnamed)";
        a.description = p->Description ? p->Description : L"";
        a.ifIndex = p->IfIndex;
        a.dhcpEnabled = (p->Flags & IP_ADAPTER_DHCP_ENABLED) != 0;
        a.connected = (p->OperStatus == IfOperStatusUp);

        if (p->PhysicalAddressLength == 6) {
            wchar_t mac[18];
            swprintf_s(mac, L"%02X-%02X-%02X-%02X-%02X-%02X",
                       p->PhysicalAddress[0], p->PhysicalAddress[1], p->PhysicalAddress[2],
                       p->PhysicalAddress[3], p->PhysicalAddress[4], p->PhysicalAddress[5]);
            a.macAddress = mac;
        }

        for (auto pu = p->FirstUnicastAddress; pu != nullptr; pu = pu->Next) {
            if (pu->Address.lpSockaddr->sa_family == AF_INET) {
                char ip[INET_ADDRSTRLEN] = {0};
                auto sin = reinterpret_cast<sockaddr_in*>(pu->Address.lpSockaddr);
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                a.ipAddress = AnsiToWide(ip, CP_ACP);
                break; // first IPv4 is enough for diagnostics purposes
            }
        }

        for (auto pg = p->FirstGatewayAddress; pg != nullptr; pg = pg->Next) {
            if (pg->Address.lpSockaddr->sa_family == AF_INET) {
                char ip[INET_ADDRSTRLEN] = {0};
                auto sin = reinterpret_cast<sockaddr_in*>(pg->Address.lpSockaddr);
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                a.gateway = AnsiToWide(ip, CP_ACP);
                break;
            }
        }

        for (auto pd = p->FirstDnsServerAddress; pd != nullptr; pd = pd->Next) {
            if (pd->Address.lpSockaddr->sa_family == AF_INET) {
                char ip[INET_ADDRSTRLEN] = {0};
                auto sin = reinterpret_cast<sockaddr_in*>(pd->Address.lpSockaddr);
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                a.dnsServers.push_back(AnsiToWide(ip, CP_ACP));
            }
        }

        m_adapters.push_back(a);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Ping / resolution helpers
// ---------------------------------------------------------------------------
bool DiagnosticsEngine::PingHost(const std::wstring& hostOrIp, DWORD timeoutMs) {
    if (hostOrIp.empty()) return false;

    IN_ADDR addr{};
    std::string ansi = WideToUtf8(hostOrIp);
    if (inet_pton(AF_INET, ansi.c_str(), &addr) != 1) {
        // Not a literal IP - try to resolve it (still a purely local operation
        // if it resolves via hosts file / local DNS cache; may fail offline, which is fine).
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (getaddrinfo(ansi.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
        addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) return false;

    char sendData[] = "WinDiagPro-ping";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 8;
    std::unique_ptr<BYTE[]> reply(new BYTE[replySize]);

    DWORD n = IcmpSendEcho(hIcmp, addr.S_un.S_addr, sendData, sizeof(sendData),
                            nullptr, reply.get(), replySize, timeoutMs);
    IcmpCloseHandle(hIcmp);
    return n > 0;
}

bool DiagnosticsEngine::CanResolve(const std::wstring& hostname) {
    std::string ansi = WideToUtf8(hostname);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    bool ok = (getaddrinfo(ansi.c_str(), nullptr, &hints, &res) == 0);
    if (res) freeaddrinfo(res);
    return ok;
}

// ---------------------------------------------------------------------------
// Network checks
// ---------------------------------------------------------------------------
std::vector<CheckResult> DiagnosticsEngine::RunNetworkChecks() {
    std::vector<CheckResult> out;

    if (!EnumerateAdapters()) {
        CheckResult r;
        r.name = L"Network Adapter Enumeration";
        r.category = DiagCategory::Network;
        r.status = DiagStatus::Fail;
        r.severity = Severity::High;
        r.details = L"GetAdaptersAddresses failed.";
        r.recommendation = L"Check that network drivers are installed and the Network Location Awareness service is running.";
        out.push_back(r);
        return out;
    }

    bool anyConnected = false;
    bool anyGatewayReachable = false;
    bool anyDhcpMissingIp = false;

    for (const auto& a : m_adapters) {
        CheckResult r;
        r.name = L"Adapter: " + a.name;
        r.category = DiagCategory::Network;

        if (a.connected && !a.ipAddress.empty()) {
            anyConnected = true;
            r.status = DiagStatus::Pass;
            r.severity = Severity::Low;
            r.details = L"Connected. IP " + a.ipAddress + (a.dhcpEnabled ? L" (DHCP)" : L" (static)");
        } else if (!a.connected) {
            r.status = DiagStatus::Warning;
            r.severity = Severity::Medium;
            r.details = L"Adapter is present but not connected (cable unplugged / Wi-Fi off / disabled).";
            r.recommendation = L"Check the physical connection, Wi-Fi toggle, or enable the adapter in Network Connections.";
        } else { // connected but no IP
            anyDhcpMissingIp = a.dhcpEnabled;
            r.status = DiagStatus::Fail;
            r.severity = Severity::High;
            r.details = L"Adapter reports connected but has no IPv4 address.";
            r.recommendation = a.dhcpEnabled
                ? L"Renew the DHCP lease (Repair tab -> Renew DHCP Lease)."
                : L"Check the static IP configuration for this adapter.";
        }
        out.push_back(r);

        if (a.connected && !a.gateway.empty()) {
            CheckResult gw;
            gw.name = L"Gateway reachability (" + a.name + L")";
            gw.category = DiagCategory::Network;
            bool reachable = PingHost(a.gateway, 1500);
            anyGatewayReachable = anyGatewayReachable || reachable;
            gw.status = reachable ? DiagStatus::Pass : DiagStatus::Fail;
            gw.severity = reachable ? Severity::Low : Severity::High;
            gw.details = a.gateway + (reachable ? L" is reachable." : L" did not respond to ping.");
            if (!reachable) {
                gw.recommendation = L"Check the router/gateway is powered on and the cable/Wi-Fi link is up.";
            }
            out.push_back(gw);
        }

        for (const auto& dns : a.dnsServers) {
            CheckResult d;
            d.name = L"DNS server " + dns + L" (" + a.name + L")";
            d.category = DiagCategory::Network;
            bool reachable = PingHost(dns, 1500);
            d.status = reachable ? DiagStatus::Pass : DiagStatus::Warning;
            d.severity = reachable ? Severity::Low : Severity::Medium;
            d.details = reachable ? L"Reachable." : L"Did not respond to ping (some DNS servers block ICMP - this is not conclusive on its own).";
            out.push_back(d);
        }
    }

    // Winsock catalog sanity check
    {
        DWORD bufLen = 0;
        int errProbe = 0;
        WSCEnumProtocols(nullptr, nullptr, &bufLen, &errProbe);
        std::vector<BYTE> buf(bufLen > 0 ? bufLen : 4096);
        DWORD outLen = (DWORD)buf.size();
        int err = 0;
        int count = WSCEnumProtocols(nullptr, reinterpret_cast<LPWSAPROTOCOL_INFOW>(buf.data()), &outLen, &err);

        CheckResult r;
        r.name = L"Winsock catalog";
        r.category = DiagCategory::Network;
        if (count > 0) {
            r.status = DiagStatus::Pass;
            r.severity = Severity::Low;
            r.details = std::to_wstring(count) + L" protocol entries registered.";
        } else {
            r.status = DiagStatus::Fail;
            r.severity = Severity::High;
            r.details = L"No usable protocol entries found in the Winsock catalog.";
            r.recommendation = L"Reset Winsock (Repair tab -> Reset Winsock), then restart the computer.";
        }
        out.push_back(r);
    }

    // Optional, informational-only internet reachability probe. Deliberately NOT
    // marked FAIL: a machine can be perfectly healthy and intentionally offline.
    {
        CheckResult r;
        r.name = L"Internet reachability (informational)";
        r.category = DiagCategory::Network;
        bool ok = anyGatewayReachable && PingHost(L"1.1.1.1", 1200);
        r.status = ok ? DiagStatus::Pass : DiagStatus::Warning;
        r.severity = Severity::Low;
        r.details = ok
            ? L"A public IP responded - this machine currently has a path to the internet."
            : L"No response from a public IP. This is expected if this machine is intentionally offline/air-gapped; otherwise check your router's WAN connection.";
        out.push_back(r);
    }

    (void)anyConnected;
    (void)anyDhcpMissingIp;
    return out;
}

// ---------------------------------------------------------------------------
// System checks
// ---------------------------------------------------------------------------
static std::vector<CheckResult> CheckServiceState(const std::wstring& serviceName,
                                                    const std::wstring& friendlyName,
                                                    bool optionalIfMissing) {
    std::vector<CheckResult> out;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        CheckResult r;
        r.name = L"Service Control Manager";
        r.category = DiagCategory::System;
        r.status = DiagStatus::Fail;
        r.severity = Severity::High;
        r.details = L"Unable to open the Service Control Manager.";
        r.recommendation = L"Run WinDiagPro as Administrator.";
        out.push_back(r);
        return out;
    }

    SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_QUERY_STATUS);
    if (!svc) {
        CheckResult r;
        r.name = L"Service: " + friendlyName;
        r.category = DiagCategory::System;
        if (optionalIfMissing) {
            r.status = DiagStatus::Info;
            r.severity = Severity::Low;
            r.details = L"Not installed on this system (likely not applicable to this hardware/edition).";
        } else {
            r.status = DiagStatus::Warning;
            r.severity = Severity::Medium;
            r.details = L"Service not found.";
        }
        out.push_back(r);
        CloseServiceHandle(scm);
        return out;
    }

    SERVICE_STATUS status{};
    CheckResult r;
    r.name = L"Service: " + friendlyName;
    r.category = DiagCategory::System;
    if (QueryServiceStatus(svc, &status)) {
        bool running = (status.dwCurrentState == SERVICE_RUNNING);
        r.status = running ? DiagStatus::Pass : DiagStatus::Fail;
        r.severity = running ? Severity::Low : Severity::High;
        r.details = running ? L"Running." : L"Not running.";
        if (!running) {
            r.recommendation = L"Start the service (Repair tab -> Restart Service).";
        }
    } else {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.details = L"Could not query status.";
    }
    out.push_back(r);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return out;
}

std::vector<CheckResult> DiagnosticsEngine::RunSystemChecks() {
    std::vector<CheckResult> out;

    struct SvcDef { const wchar_t* svc; const wchar_t* friendly; bool optional; };
    static const SvcDef services[] = {
        { L"Dhcp",      L"DHCP Client",              false },
        { L"Dnscache",  L"DNS Client",               false },
        { L"NlaSvc",    L"Network Location Awareness", false },
        { L"WlanSvc",   L"WLAN AutoConfig",          true  }, // absent on wired-only desktops
        { L"BFE",       L"Base Filtering Engine",    false },
        { L"MpsSvc",    L"Windows Firewall",         false },
        { L"Winmgmt",   L"Windows Management Instrumentation", false },
        { L"wuauserv",  L"Windows Update",           false },
    };
    for (auto& s : services) {
        auto r = CheckServiceState(s.svc, s.friendly, s.optional);
        out.insert(out.end(), r.begin(), r.end());
    }

    // Disk free space on the system drive
    {
        wchar_t sysDir[MAX_PATH];
        GetWindowsDirectoryW(sysDir, MAX_PATH);
        wchar_t drive[4] = { sysDir[0], L':', L'\\', 0 };

        ULARGE_INTEGER freeBytes{}, totalBytes{};
        CheckResult r;
        r.name = L"System drive free space";
        r.category = DiagCategory::System;
        if (GetDiskFreeSpaceExW(drive, &freeBytes, &totalBytes, nullptr)) {
            double freeGB = (double)freeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
            double totalGB = (double)totalBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
            double pctFree = totalGB > 0 ? (freeGB / totalGB * 100.0) : 0;

            wchar_t buf[128];
            swprintf_s(buf, L"%.1f GB free of %.1f GB (%.0f%% free) on %s",
                       freeGB, totalGB, pctFree, drive);
            r.details = buf;

            if (pctFree < 5.0) {
                r.status = DiagStatus::Fail;
                r.severity = Severity::Critical;
                r.recommendation = L"Free up disk space immediately - low disk space can cause update, hibernation, and application failures.";
            } else if (pctFree < 15.0) {
                r.status = DiagStatus::Warning;
                r.severity = Severity::Medium;
                r.recommendation = L"Consider freeing up disk space (Disk Cleanup / Storage Sense).";
            } else {
                r.status = DiagStatus::Pass;
                r.severity = Severity::Low;
            }
        } else {
            r.status = DiagStatus::Warning;
            r.severity = Severity::Medium;
            r.details = L"Could not query free disk space.";
        }
        out.push_back(r);
    }

    // Recent Critical/Error events in the System log (last 24h) via the modern Event Log API.
    {
        CheckResult r;
        r.name = L"Recent System log errors (24h)";
        r.category = DiagCategory::System;

        const wchar_t* query =
            L"*[System[(Level=1 or Level=2) and TimeCreated[timediff(@SystemTime) <= 86400000]]]";
        EVT_HANDLE hResults = EvtQuery(nullptr, L"System", query,
                                        EvtQueryChannelPath | EvtQueryReverseDirection);
        if (hResults) {
            DWORD count = 0;
            EVT_HANDLE events[1] = { nullptr };
            DWORD returned = 0;
            while (EvtNext(hResults, 1, events, INFINITE, 0, &returned) && returned > 0) {
                count++;
                EvtClose(events[0]);
                if (count >= 500) break; // sanity cap
            }
            EvtClose(hResults);

            r.details = std::to_wstring(count) + L" Critical/Error events in the last 24 hours.";
            if (count == 0) {
                r.status = DiagStatus::Pass; r.severity = Severity::Low;
            } else if (count < 10) {
                r.status = DiagStatus::Warning; r.severity = Severity::Medium;
                r.recommendation = L"Review Event Viewer > Windows Logs > System for details.";
            } else {
                r.status = DiagStatus::Fail; r.severity = Severity::High;
                r.recommendation = L"A high volume of system errors was logged recently - review Event Viewer > Windows Logs > System.";
            }
        } else {
            r.status = DiagStatus::Warning;
            r.severity = Severity::Low;
            r.details = L"Could not query the Windows Event Log (may require Administrator).";
        }
        out.push_back(r);
    }

    return out;
}

// ---------------------------------------------------------------------------
// Hardware checks (WMI, local only)
// ---------------------------------------------------------------------------
std::vector<CheckResult> DiagnosticsEngine::RunHardwareChecks() {
    std::vector<CheckResult> out;

    if (!m_wmi.IsReady()) {
        CheckResult r;
        r.name = L"Hardware inventory (WMI)";
        r.category = DiagCategory::Hardware;
        r.status = DiagStatus::Warning;
        r.severity = Severity::Low;
        r.details = L"WMI is unavailable - skipping detailed hardware checks.";
        out.push_back(r);
        return out;
    }

    // Devices Windows currently flags as having a problem (ConfigManagerErrorCode != 0).
    auto rows = m_wmi.Query(
        L"SELECT Name, DeviceID, ConfigManagerErrorCode FROM Win32_PnPEntity "
        L"WHERE ConfigManagerErrorCode != 0");

    if (rows.empty()) {
        CheckResult r;
        r.name = L"Device Manager problem devices";
        r.category = DiagCategory::Hardware;
        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
        r.details = L"No devices reporting a Device Manager error code.";
        out.push_back(r);
    } else {
        for (auto& row : rows) {
            CheckResult r;
            std::wstring name = row.count(L"Name") ? row[L"Name"] : L"(unknown device)";
            std::wstring code = row.count(L"ConfigManagerErrorCode") ? row[L"ConfigManagerErrorCode"] : L"?";
            r.name = L"Problem device: " + name;
            r.category = DiagCategory::Hardware;
            r.status = DiagStatus::Fail;
            r.severity = Severity::High;
            r.details = L"Device Manager error code " + code + L".";
            r.recommendation = L"Open Device Manager, update or reinstall the driver for this device, or run its manufacturer's driver installer.";
            out.push_back(r);
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Security checks
// ---------------------------------------------------------------------------
std::vector<CheckResult> DiagnosticsEngine::RunSecurityChecks() {
    std::vector<CheckResult> out;

    // Windows Firewall profile status via the local firewall COM API (no network access).
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool didInit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) hr = S_OK; // already initialized elsewhere, fine

    INetFwPolicy2* pPolicy = nullptr;
    HRESULT hrc = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                    __uuidof(INetFwPolicy2), (void**)&pPolicy);
    if (SUCCEEDED(hrc) && pPolicy) {
        struct ProfileDef { NET_FW_PROFILE_TYPE2 type; const wchar_t* name; };
        static const ProfileDef profiles[] = {
            { NET_FW_PROFILE2_DOMAIN,  L"Domain" },
            { NET_FW_PROFILE2_PRIVATE, L"Private" },
            { NET_FW_PROFILE2_PUBLIC,  L"Public" },
        };
        for (auto& p : profiles) {
            VARIANT_BOOL enabled = VARIANT_FALSE;
            CheckResult r;
            r.name = std::wstring(L"Firewall profile: ") + p.name;
            r.category = DiagCategory::Security;
            if (SUCCEEDED(pPolicy->get_FirewallEnabled(p.type, &enabled)) && enabled == VARIANT_TRUE) {
                r.status = DiagStatus::Pass;
                r.severity = Severity::Low;
                r.details = L"Enabled.";
            } else {
                r.status = DiagStatus::Warning;
                r.severity = Severity::Medium;
                r.details = L"Disabled.";
                r.recommendation = L"Consider re-enabling the Windows Firewall for this profile unless a managed security product replaces it.";
            }
            out.push_back(r);
        }
        pPolicy->Release();
    } else {
        CheckResult r;
        r.name = L"Windows Firewall profiles";
        r.category = DiagCategory::Security;
        r.status = DiagStatus::Warning;
        r.severity = Severity::Low;
        r.details = L"Could not query firewall profile status.";
        out.push_back(r);
    }
    if (didInit) CoUninitialize();

    // Windows Defender antivirus service (real-time protection service presence/state).
    auto defender = CheckServiceState(L"WinDefend", L"Windows Defender Antivirus", true);
    out.insert(out.end(), defender.begin(), defender.end());

    return out;
}

// ---------------------------------------------------------------------------
// Slow / on-demand checks
// ---------------------------------------------------------------------------
std::vector<CheckResult> DiagnosticsEngine::RunSFCScan() {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"System File Checker (sfc /scannow)";
    r.category = DiagCategory::System;

    if (!IsElevated()) {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.details = L"Administrator privileges are required to run SFC.";
        out.push_back(r);
        return out;
    }

    std::wstring output = RunCommandCaptureOutput(L"sfc /scannow", 30 * 60 * 1000);
    r.details = output.substr(0, std::min<size_t>(output.size(), 2000));

    if (output.find(L"did not find any integrity violations") != std::wstring::npos) {
        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
    } else if (output.find(L"found corrupt files") != std::wstring::npos &&
               output.find(L"successfully repaired") != std::wstring::npos) {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.recommendation = L"Corrupt files were found and repaired. A restart is recommended.";
    } else if (output.find(L"found corrupt files") != std::wstring::npos) {
        r.status = DiagStatus::Fail;
        r.severity = Severity::High;
        r.recommendation = L"Corrupt files were found but not all could be repaired. Try DISM /RestoreHealth, then re-run SFC.";
    } else {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.recommendation = L"SFC output was not recognized - review the full log in the Report tab.";
    }
    out.push_back(r);
    return out;
}

static std::vector<CheckResult> RunDismStage(const std::wstring& args, const std::wstring& label) {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"DISM " + label;
    r.category = DiagCategory::System;

    if (!IsElevated()) {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.details = L"Administrator privileges are required to run DISM.";
        out.push_back(r);
        return out;
    }

    std::wstring output = RunCommandCaptureOutput(L"DISM /Online /Cleanup-Image " + args, 30 * 60 * 1000);
    r.details = output.substr(0, std::min<size_t>(output.size(), 2000));

    if (output.find(L"No component store corruption detected") != std::wstring::npos ||
        output.find(L"image is healthy") != std::wstring::npos) {
        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
    } else if (output.find(L"repairable") != std::wstring::npos) {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.recommendation = L"Run DISM /Online /Cleanup-Image /RestoreHealth from the Repair tab.";
    } else if (output.find(L"restored") != std::wstring::npos || output.find(L"operation completed successfully") != std::wstring::npos) {
        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
    } else {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.recommendation = L"Review the full DISM log in the Report tab.";
    }
    out.push_back(r);
    return out;
}

std::vector<CheckResult> DiagnosticsEngine::RunDISMCheckHealth() {
    return RunDismStage(L"/CheckHealth", L"CheckHealth");
}

std::vector<CheckResult> DiagnosticsEngine::RunDISMScanHealth() {
    return RunDismStage(L"/ScanHealth", L"ScanHealth");
}
