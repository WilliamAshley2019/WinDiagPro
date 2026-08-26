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
#include <sstream>
#include <map>
#include <chrono>

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
namespace {

// GetAdaptersAddresses' FirstGatewayAddress field is unreliable in practice
// for IPv4 gateways learned via plain DHCP - it can (and does, confirmed by
// real-world reports) come back empty even though the adapter has a fully
// working gateway that ipconfig shows correctly. ipconfig doesn't rely
// solely on that field; it effectively cross-references the IP routing
// table. This does the same: look up the adapter's 0.0.0.0/0 default route
// directly, which is authoritative (it's what actually determines traffic
// behavior) rather than a possibly-stale adapter property.
std::wstring FindDefaultGatewayFromRouteTable(DWORD ifIndex) {
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR || !table) return L"";

    std::wstring gateway;
    ULONG bestMetric = 0;
    bool found = false;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        auto& row = table->Table[i];
        if (row.InterfaceIndex != ifIndex) continue;
        if (row.DestinationPrefix.PrefixLength != 0) continue; // only the default route (0.0.0.0/0)
        if (row.DestinationPrefix.Prefix.si_family != AF_INET) continue;

        auto& nh = row.NextHop.Ipv4.sin_addr;
        if (nh.S_un.S_addr == 0) continue; // an all-zero next hop isn't a real gateway

        if (!found || row.Metric < bestMetric) {
            char buf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &nh, buf, sizeof(buf));
            gateway = AnsiToWide(buf, CP_ACP);
            bestMetric = row.Metric;
            found = true;
        }
    }
    FreeMibTable(table);
    return gateway;
}

} // namespace

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

        // Route table first (authoritative - see FindDefaultGatewayFromRouteTable
        // comment above), adapter field only as a fallback if that comes up empty.
        a.gateway = FindDefaultGatewayFromRouteTable(a.ifIndex);
        if (a.gateway.empty()) {
            for (auto pg = p->FirstGatewayAddress; pg != nullptr; pg = pg->Next) {
                if (pg->Address.lpSockaddr->sa_family == AF_INET) {
                    char ip[INET_ADDRSTRLEN] = {0};
                    auto sin = reinterpret_cast<sockaddr_in*>(pg->Address.lpSockaddr);
                    inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                    a.gateway = AnsiToWide(ip, CP_ACP);
                    break;
                }
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
    ClassifyAdapters();
    return true;
}

std::wstring AdapterRoleToString(AdapterRole r) {
    switch (r) {
        case AdapterRole::Disconnected:         return L"Disconnected";
        case AdapterRole::Virtual:               return L"Virtual";
        case AdapterRole::GatewayScopeMismatch:   return L"Gateway scope mismatch";
        case AdapterRole::DirectLink:             return L"Direct link (APIPA)";
        case AdapterRole::Isolated:               return L"Isolated";
        case AdapterRole::Routed:                 return L"Routed";
    }
    return L"?";
}

bool DiagnosticsEngine::IsVirtualAdapterDescription(const std::wstring& desc) {
    static const wchar_t* markers[] = {
        L"Hyper-V", L"Virtual Ethernet", L"vEthernet", L"WSL", L"Virtual Switch",
        L"TAP-Windows", L"TAP-Win", L"VPN", L"WireGuard", L"Tailscale", L"ZeroTier",
        L"VMware", L"VirtualBox", L"Npcap Loopback"
    };
    for (auto m : markers) {
        if (desc.find(m) != std::wstring::npos) return true;
    }
    return false;
}

DWORD DiagnosticsEngine::GetInterfaceMetric(DWORD ifIndex) {
    MIB_IPINTERFACE_ROW row{};
    row.Family = AF_INET;
    row.InterfaceIndex = ifIndex;
    if (GetIpInterfaceEntry(&row) == NO_ERROR) {
        return row.Metric;
    }
    return 0;
}

DWORD DiagnosticsEngine::GetInterfaceMtu(DWORD ifIndex) {
    MIB_IPINTERFACE_ROW row{};
    row.Family = AF_INET;
    row.InterfaceIndex = ifIndex;
    if (GetIpInterfaceEntry(&row) == NO_ERROR) {
        return row.NlMtu;
    }
    return 0;
}

std::wstring DiagnosticsEngine::GetGatewayNudState(DWORD ifIndex, const std::wstring& gatewayIp) {
    if (gatewayIp.empty()) return L"";

    IN_ADDR target{};
    std::string ansi = WideToUtf8(gatewayIp);
    if (inet_pton(AF_INET, ansi.c_str(), &target) != 1) return L"";

    PMIB_IPNET_TABLE2 table = nullptr;
    if (GetIpNetTable2(AF_INET, &table) != NO_ERROR || !table) return L"";

    std::wstring result;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        auto& row = table->Table[i];
        if (row.InterfaceIndex != ifIndex) continue;
        auto sin = reinterpret_cast<const sockaddr_in*>(&row.Address.Ipv4);
        if (sin->sin_addr.S_un.S_addr != target.S_un.S_addr) continue;

        switch (row.State) {
            case NlnsUnreachable: result = L"Unreachable"; break;
            case NlnsIncomplete:  result = L"Incomplete (still resolving)"; break;
            case NlnsProbe:       result = L"Probe (Windows has not confirmed it can reach this gateway)"; break;
            case NlnsDelay:       result = L"Delay (waiting to re-verify)"; break;
            case NlnsStale:       result = L"Stale (worked before, not re-verified yet)"; break;
            case NlnsReachable:   result = L"Reachable"; break;
            case NlnsPermanent:   result = L"Permanent (statically configured)"; break;
            default:              result = L"Unknown"; break;
        }
        break;
    }
    FreeMibTable(table);
    return result;
}

void DiagnosticsEngine::ClassifyAdapters() {
    for (auto& a : m_adapters) {
        a.isVirtual = IsVirtualAdapterDescription(a.description) || IsVirtualAdapterDescription(a.name);
        a.metric = a.ifIndex ? GetInterfaceMetric(a.ifIndex) : 0;
        a.mtu = a.ifIndex ? GetInterfaceMtu(a.ifIndex) : 0;

        bool isApipa = a.ipAddress.rfind(L"169.254.", 0) == 0;

        if (!a.connected) {
            a.role = AdapterRole::Disconnected;
        } else if (a.isVirtual) {
            a.role = AdapterRole::Virtual;
        } else if (isApipa) {
            // A 169.254.0.0/16 address is link-local only (RFC 3927) - it must
            // never have a usable routable default gateway attached to it.
            // Seeing one anyway is a specific, fixable misconfiguration
            // distinct from "DHCP hasn't come back yet".
            bool gatewayLooksRoutable = !a.gateway.empty() && a.gateway.rfind(L"169.254.", 0) != 0;
            a.role = gatewayLooksRoutable ? AdapterRole::GatewayScopeMismatch : AdapterRole::DirectLink;
        } else if (a.ipAddress.empty() || a.gateway.empty()) {
            a.role = AdapterRole::Isolated;
        } else {
            a.role = AdapterRole::Routed;
        }

        if (!a.gateway.empty() && (a.role == AdapterRole::Routed || a.role == AdapterRole::GatewayScopeMismatch)) {
            a.gatewayNudState = GetGatewayNudState(a.ifIndex, a.gateway);
        }
    }
}

std::vector<NetworkAdapterInfo> DiagnosticsEngine::GetAdapterTopology() {
    EnumerateAdapters();
    return m_adapters;
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

    for (const auto& a : m_adapters) {
        CheckResult r;
        r.name = L"Adapter: " + a.name;
        r.category = DiagCategory::Network;

        switch (a.role) {
            case AdapterRole::Virtual:
                r.status = DiagStatus::Info;
                r.severity = Severity::Low;
                r.details = L"Virtual adapter (" + a.description + L") - not physical hardware; "
                             L"excluded from connectivity diagnosis so it can't masquerade as a "
                             L"real network problem.";
                break;

            case AdapterRole::Disconnected:
                r.status = DiagStatus::Warning;
                r.severity = Severity::Medium;
                r.details = L"Adapter is present but not connected (cable unplugged / Wi-Fi off / disabled).";
                r.recommendation = L"Check the physical connection, Wi-Fi toggle, or enable the adapter in Network Connections.";
                break;

            case AdapterRole::GatewayScopeMismatch:
                anyConnected = true;
                r.status = DiagStatus::Fail;
                r.severity = Severity::High;
                r.details = L"Has a link-local APIPA address (" + a.ipAddress + L") - meaning DHCP has "
                             L"not succeeded - but a default gateway (" + a.gateway + L") is still "
                             L"attached to it. A 169.254.x.x address must never carry a routable "
                             L"gateway (RFC 3927); this exact combination is what causes some "
                             L"connections/lookups to fail while others keep working.";
                r.recommendation = L"Right-click this row for \"Release & renew DHCP on this adapter\" "
                                    L"- don't reset Winsock/TCP-IP or flush DNS for this, the problem is "
                                    L"this one adapter's lease, not general network configuration.";
                break;

            case AdapterRole::DirectLink:
                anyConnected = true;
                r.status = DiagStatus::Info;
                r.severity = Severity::Low;
                r.details = L"Link-local address only (" + a.ipAddress + L", no gateway attached) - "
                             L"normal if this is a direct cable to another PC with no DHCP server, or "
                             L"if a DHCP server simply hasn't been found yet on this link.";
                break;

            case AdapterRole::Isolated:
                r.status = DiagStatus::Fail;
                r.severity = Severity::High;
                if (a.ipAddress.empty()) {
                    r.details = L"Adapter reports connected but has no IPv4 address at all.";
                    r.recommendation = a.dhcpEnabled
                        ? L"Right-click this row for \"Release & renew DHCP on this adapter\"."
                        : L"Check the static IP configuration for this adapter.";
                } else {
                    r.details = L"Has IP " + a.ipAddress + L" but no default gateway configured.";
                    r.recommendation = L"Expected for an isolated/local-only segment; otherwise check this adapter's IP configuration.";
                }
                break;

            case AdapterRole::Routed:
            default: {
                anyConnected = true;
                r.status = DiagStatus::Pass;
                r.severity = Severity::Low;
                r.details = L"Connected. IP " + a.ipAddress + (a.dhcpEnabled ? L" (DHCP)" : L" (static)") +
                             L", metric " + std::to_wstring(a.metric) + L", MTU " + std::to_wstring(a.mtu);
                if (!a.gatewayNudState.empty()) {
                    r.details += L". Gateway state: " + a.gatewayNudState;
                }
                break;
            }
        }
        out.push_back(r);

        if ((a.role == AdapterRole::Routed || a.role == AdapterRole::GatewayScopeMismatch) && !a.gateway.empty()) {
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

        if (a.role == AdapterRole::Virtual) continue; // skip DNS-server pings for virtual switches

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

    auto activeRouting = CheckActiveRouting();
    out.insert(out.end(), activeRouting.begin(), activeRouting.end());

    auto staticRoutes = CheckStaticRoutes();
    out.insert(out.end(), staticRoutes.begin(), staticRoutes.end());

    auto localDevices = CheckLocalDevices();
    out.insert(out.end(), localDevices.begin(), localDevices.end());

    auto dnsCache = CheckDnsCache();
    out.insert(out.end(), dnsCache.begin(), dnsCache.end());

    (void)anyConnected;
    return out;
}

std::vector<CheckResult> DiagnosticsEngine::CheckActiveRouting() {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"Active route selection";
    r.category = DiagCategory::Network;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    inet_pton(AF_INET, "1.1.1.1", &dest.sin_addr);

    DWORD bestIfIndex = 0;
    DWORD apiResult = GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&dest), &bestIfIndex);

    if (apiResult != NO_ERROR) {
        r.status = DiagStatus::Info;
        r.severity = Severity::Low;
        r.details = L"Could not determine which adapter Windows would use for general internet traffic.";
    } else {
        std::wstring adapterName;
        for (auto& a : m_adapters) {
            if (a.ifIndex == bestIfIndex) { adapterName = a.name; break; }
        }
        if (adapterName.empty()) adapterName = L"(interface index " + std::to_wstring(bestIfIndex) + L")";

        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
        r.details = L"Windows would currently route general internet traffic through: " + adapterName +
                    L". This is a direct query (GetBestInterfaceEx), not an inference from metric "
                    L"numbers - it's the definitive answer to \"which adapter actually wins\" on a "
                    L"multi-adapter machine, rather than something to infer from the metric column.";
    }
    out.push_back(r);
    return out;
}

std::vector<CheckResult> DiagnosticsEngine::CheckStaticRoutes() {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"Manually-configured routes";
    r.category = DiagCategory::Network;

    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR || !table) {
        r.status = DiagStatus::Info;
        r.severity = Severity::Low;
        r.details = L"Could not read the IPv4 routing table.";
        out.push_back(r);
        return out;
    }

    std::vector<std::wstring> manualRoutes;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        auto& row = table->Table[i];
        if (row.Origin != NlroManual) continue; // skip normal auto-generated (DHCP/well-known/RA/6to4) routes

        char destBuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &row.DestinationPrefix.Prefix.Ipv4.sin_addr, destBuf, sizeof(destBuf));
        char nextHopBuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &row.NextHop.Ipv4.sin_addr, nextHopBuf, sizeof(nextHopBuf));

        std::wstring adapterName;
        for (auto& a : m_adapters) {
            if (a.ifIndex == row.InterfaceIndex) { adapterName = a.name; break; }
        }
        if (adapterName.empty()) adapterName = L"if#" + std::to_wstring(row.InterfaceIndex);

        std::wstring line = AnsiToWide(destBuf, CP_ACP) + L"/" +
                             std::to_wstring(row.DestinationPrefix.PrefixLength) + L" via " +
                             AnsiToWide(nextHopBuf, CP_ACP) + L" on " + adapterName +
                             L" (metric " + std::to_wstring(row.Metric) + L")";
        manualRoutes.push_back(line);
    }
    FreeMibTable(table);

    if (manualRoutes.empty()) {
        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
        r.details = L"No manually-configured (static) IPv4 routes found - only normal auto-generated ones.";
    } else {
        r.status = DiagStatus::Info;
        r.severity = Severity::Medium;
        std::wstring joined;
        for (size_t i = 0; i < manualRoutes.size(); ++i) {
            joined += manualRoutes[i];
            if (i + 1 < manualRoutes.size()) joined += L" | ";
        }
        r.details = std::to_wstring(manualRoutes.size()) + L" manual route(s) found: " + joined +
                    L". These override normal metric-based adapter selection for their specific "
                    L"destination - worth knowing about if traffic to a particular address behaves "
                    L"unexpectedly (VPN clients and some corporate software add these automatically).";
        r.recommendation = L"Review with 'route print' in an elevated Command Prompt (Help tab has a "
                            L"quick-launch button) if any of these are unexpected; remove with "
                            L"'route delete <destination>'.";
    }
    out.push_back(r);
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

    auto timeSync = CheckTimeSync();
    out.insert(out.end(), timeSync.begin(), timeSync.end());

    auto restore = CheckSystemRestore();
    out.insert(out.end(), restore.begin(), restore.end());

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

    auto hosts = CheckHostsFile();
    out.insert(out.end(), hosts.begin(), hosts.end());

    auto proxy = CheckProxySettings();
    out.insert(out.end(), proxy.begin(), proxy.end());

    auto autostart = CheckAutostartEntries();
    out.insert(out.end(), autostart.begin(), autostart.end());

    return out;
}

// ---------------------------------------------------------------------------
// Checks specifically for "the network/internet looks fine but browsing
// still doesn't work" - each targets a classic cause that adapter/gateway/
// DNS-server checks alone completely miss.
// ---------------------------------------------------------------------------
std::vector<CheckResult> DiagnosticsEngine::CheckHostsFile() {
    std::vector<CheckResult> out;

    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring hostsPath = std::wstring(sysDir) + L"\\drivers\\etc\\hosts";

    HANDLE h = CreateFileW(hostsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CheckResult r;
    r.name = L"Hosts file";
    r.category = DiagCategory::Security;

    if (h == INVALID_HANDLE_VALUE) {
        r.status = DiagStatus::Info;
        r.severity = Severity::Low;
        r.details = L"Could not read " + hostsPath + L" (may require Administrator).";
        out.push_back(r);
        return out;
    }

    DWORD size = GetFileSize(h, nullptr);
    std::string raw;
    if (size != INVALID_FILE_SIZE && size > 0 && size < 5 * 1024 * 1024) {
        raw.resize(size);
        DWORD bytesRead = 0;
        ReadFile(h, raw.data(), size, &bytesRead, nullptr);
        raw.resize(bytesRead);
    }
    CloseHandle(h);

    std::wstring content = AnsiToWide(raw, CP_ACP);
    std::vector<std::wstring> activeEntries;
    std::wstringstream ss(content);
    std::wstring line;
    while (std::getline(ss, line)) {
        // Trim trailing \r and surrounding whitespace.
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        size_t start = line.find_first_not_of(L" \t");
        if (start == std::wstring::npos) continue; // blank line
        line = line.substr(start);
        if (line.empty() || line[0] == L'#') continue; // comment

        // Ignore the standard loopback-to-localhost entries; anything else is
        // a real, active hostname redirection worth a human looking at.
        bool isStandardLoopback =
            (line.find(L"127.0.0.1") == 0 || line.find(L"::1") == 0) &&
            line.find(L"localhost") != std::wstring::npos;
        if (isStandardLoopback) continue;

        activeEntries.push_back(line);
    }

    if (activeEntries.empty()) {
        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
        r.details = L"No unexpected entries - only comments/blank lines (or standard localhost mappings).";
    } else {
        r.status = DiagStatus::Warning;
        r.severity = Severity::High;
        std::wstring joined;
        for (size_t i = 0; i < activeEntries.size() && i < 10; ++i) {
            joined += activeEntries[i];
            if (i + 1 < activeEntries.size()) joined += L" | ";
        }
        r.details = std::to_wstring(activeEntries.size()) + L" active entry/entries redirecting specific "
                    L"hostnames: " + joined +
                    L". This is a classic cause of \"some websites won't load/resolve while others "
                    L"work fine\" - either intentional (ad-blocking, dev/testing) or a sign of "
                    L"tampering/malware if you don't recognize these.";
        r.recommendation = L"Right-click this row to open the hosts file in Notepad and review it. "
                            L"Remove any line you don't recognize, save, then re-run this check.";
        out.push_back(r);
        return out;
    }
    out.push_back(r);
    return out;
}

std::vector<CheckResult> DiagnosticsEngine::CheckProxySettings() {
    std::vector<CheckResult> out;

    // WinINet (browser/most desktop apps) proxy - HKCU, no admin required to read.
    {
        HKEY key = nullptr;
        CheckResult r;
        r.name = L"Browser/app proxy (WinINet)";
        r.category = DiagCategory::Security;

        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                           L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                           0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD proxyEnable = 0, size = sizeof(proxyEnable);
            RegGetValueW(key, nullptr, L"ProxyEnable", RRF_RT_REG_DWORD, nullptr, &proxyEnable, &size);

            if (proxyEnable != 0) {
                wchar_t serverBuf[512] = {0};
                DWORD serverSize = sizeof(serverBuf);
                RegGetValueW(key, nullptr, L"ProxyServer", RRF_RT_REG_SZ, nullptr, serverBuf, &serverSize);
                r.status = DiagStatus::Warning;
                r.severity = Severity::Medium;
                r.details = L"A proxy is configured and enabled: " + std::wstring(serverBuf) +
                            L". If this proxy server is unreachable or misconfigured, web browsing "
                            L"and app updates will fail even though the network/DNS/gateway are fine.";
                r.recommendation = L"If you don't intentionally use a proxy, right-click this row to open "
                                    L"proxy settings and disable it.";
            } else {
                r.status = DiagStatus::Pass;
                r.severity = Severity::Low;
                r.details = L"No browser/app proxy configured (direct connection).";
            }
            RegCloseKey(key);
        } else {
            r.status = DiagStatus::Info;
            r.severity = Severity::Low;
            r.details = L"Could not read proxy configuration.";
        }
        out.push_back(r);
    }

    // WinHTTP proxy - separate from WinINet above; used by Windows Update,
    // many Windows services, and some line-of-business apps. A stale WinHTTP
    // proxy (often left behind by old corporate/VPN software) is a very
    // common, easily-missed cause of "Windows Update is stuck" or "some
    // services can't reach the internet but my browser works fine".
    {
        CheckResult r;
        r.name = L"System proxy (WinHTTP)";
        r.category = DiagCategory::Security;
        std::wstring output = RunCommandCaptureOutput(L"netsh winhttp show proxy", 10000);

        if (output.find(L"Direct access") != std::wstring::npos) {
            r.status = DiagStatus::Pass;
            r.severity = Severity::Low;
            r.details = L"No system-wide (WinHTTP) proxy configured.";
        } else if (output.find(L"Proxy Server") != std::wstring::npos) {
            r.status = DiagStatus::Warning;
            r.severity = Severity::Medium;
            r.details = L"A system-wide WinHTTP proxy is configured. This affects Windows Update and "
                        L"many services directly. Output: " + output.substr(0, 300);
            r.recommendation = L"If this wasn't set up intentionally (e.g. leftover from old VPN/corporate "
                                L"software), use the \"Reset WinHTTP Proxy\" repair action.";
        } else {
            r.status = DiagStatus::Info;
            r.severity = Severity::Low;
            r.details = L"Could not determine WinHTTP proxy state.";
        }
        out.push_back(r);
    }

    return out;
}

std::vector<CheckResult> DiagnosticsEngine::CheckTimeSync() {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"System clock / time sync";
    r.category = DiagCategory::System;

    auto svcCheck = CheckServiceState(L"W32Time", L"Windows Time", true);
    bool serviceRunning = !svcCheck.empty() && svcCheck[0].status == DiagStatus::Pass;

    std::wstring output = RunCommandCaptureOutput(L"w32tm /query /status", 10000);
    bool neverSynced = output.find(L"has not yet synchronized") != std::wstring::npos ||
                        output.empty();

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t dateBuf[64];
    swprintf_s(dateBuf, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

    if (!serviceRunning) {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.details = L"Windows Time service is not running. Current system clock: " + std::wstring(dateBuf) +
                    L". An incorrect clock is a very common, easy-to-miss cause of HTTPS/TLS sites "
                    L"failing to load (certificate validation depends on the clock being roughly "
                    L"correct) even when the network itself is completely fine.";
        r.recommendation = L"Right-click to open Date & Time settings and verify the date/time are "
                            L"correct, or restart the Windows Time service.";
    } else if (neverSynced) {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Low;
        r.details = L"Windows Time service is running but reports it has not yet successfully "
                    L"synchronized. Current system clock: " + std::wstring(dateBuf) +
                    L". Worth double-checking the date/time are correct if HTTPS sites are failing "
                    L"to load with certificate errors.";
        r.recommendation = L"Verify the date/time in Settings, or run: w32tm /resync (needs a working "
                            L"time source, which requires network/domain connectivity).";
    } else {
        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
        r.details = L"Windows Time service is running and has synchronized. Current system clock: " +
                    std::wstring(dateBuf);
    }
    out.push_back(r);
    return out;
}

std::vector<CheckResult> DiagnosticsEngine::CheckSystemRestore() {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"System Restore points";
    r.category = DiagCategory::System;

    std::wstring countOutput = RunCommandCaptureOutput(
        L"powershell -NoProfile -NonInteractive -Command "
        L"\"(Get-ComputerRestorePoint | Measure-Object).Count\"", 20000);

    int count = -1;
    size_t digitStart = countOutput.find_first_of(L"0123456789");
    if (digitStart != std::wstring::npos) {
        try { count = std::stoi(countOutput.substr(digitStart)); } catch (...) { count = -1; }
    }

    if (count < 0) {
        r.status = DiagStatus::Info;
        r.severity = Severity::Low;
        r.details = L"Could not determine System Restore status (it may be disabled, or unavailable "
                    L"on this Windows edition/policy).";
        r.recommendation = L"Right-click to open System Restore directly and check/enable it there.";
    } else if (count == 0) {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.details = L"No System Restore points found.";
        r.recommendation = L"Consider creating one (right-click this row) before making system changes - "
                            L"it's the single best whole-system undo button if a repair goes wrong, not "
                            L"just a network/driver rollback.";
    } else {
        std::wstring latest = RunCommandCaptureOutput(
            L"powershell -NoProfile -NonInteractive -Command "
            L"\"$p = Get-ComputerRestorePoint | Sort-Object SequenceNumber -Descending | "
            L"Select-Object -First 1; if ($p) { $p.CreationTime.ToString() + ' - ' + $p.Description }\"",
            20000);
        while (!latest.empty() && (latest.back() == L'\r' || latest.back() == L'\n' || latest.back() == L' '))
            latest.pop_back();
        size_t s = latest.find_first_not_of(L" \r\n");
        latest = (s != std::wstring::npos) ? latest.substr(s) : L"";

        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
        r.details = std::to_wstring(count) + L" restore point(s) found." +
                     (latest.empty() ? L"" : (L" Most recent: " + latest));
    }
    out.push_back(r);
    return out;
}

std::vector<CheckResult> DiagnosticsEngine::CheckAutostartEntries() {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"Autostart programs";
    r.category = DiagCategory::Security;

    struct RunKey { HKEY root; const wchar_t* path; };
    static const RunKey keys[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce" },
    };

    std::vector<std::wstring> names;
    for (auto& k : keys) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(k.root, k.path, 0, KEY_READ, &hKey) != ERROR_SUCCESS) continue;

        DWORD index = 0;
        for (;;) {
            wchar_t valueName[256];
            DWORD nameSize = 256;
            LONG res = RegEnumValueW(hKey, index, valueName, &nameSize, nullptr, nullptr, nullptr, nullptr);
            if (res == ERROR_NO_MORE_ITEMS) break;
            if (res == ERROR_SUCCESS) names.push_back(valueName);
            ++index;
        }
        RegCloseKey(hKey);
    }

    r.status = DiagStatus::Info;
    r.severity = Severity::Low;
    if (names.empty()) {
        r.details = L"No registry-based autostart entries found.";
    } else {
        std::wstring joined;
        for (size_t i = 0; i < names.size() && i < 20; ++i) {
            joined += names[i];
            if (i + 1 < names.size() && i < 19) joined += L", ";
        }
        r.details = std::to_wstring(names.size()) + L" autostart program(s): " + joined +
                    L". Worth reviewing anything you don't recognize - malware sometimes reapplies "
                    L"proxy/hosts-file/DNS changes on every boot via an autostart entry, which is why "
                    L"a fix can seem to \"come back\" after a restart.";
        r.recommendation = L"Right-click this row to open Task Manager's Startup tab for full details "
                            L"and to disable anything unfamiliar.";
    }
    out.push_back(r);
    return out;
}

// ---------------------------------------------------------------------------
// Traceroute / path-beyond-the-gateway (on-demand - not part of auto scans)
// ---------------------------------------------------------------------------
namespace {

bool IsIPv4Address(const std::wstring& s, IN_ADDR* outAddr = nullptr) {
    std::string ansi = WideToUtf8(s);
    IN_ADDR addr{};
    if (inet_pton(AF_INET, ansi.c_str(), &addr) != 1) return false;
    if (outAddr) *outAddr = addr;
    return true;
}

// RFC 6598: 100.64.0.0/10 - reserved specifically for carrier-grade NAT.
// Seeing this address on the path (rather than on your own adapter) is
// about as close to a definitive "yes, your ISP uses CGNAT here" signal as
// you can get without querying the ISP directly.
bool IsCgnatAddress(const std::wstring& ip) {
    IN_ADDR addr{};
    if (!IsIPv4Address(ip, &addr)) return false;
    BYTE b1 = addr.S_un.S_un_b.s_b1;
    BYTE b2 = addr.S_un.S_un_b.s_b2;
    return b1 == 100 && b2 >= 64 && b2 <= 127;
}

bool IsPrivateAddress(const std::wstring& ip) {
    IN_ADDR addr{};
    if (!IsIPv4Address(ip, &addr)) return false;
    BYTE b1 = addr.S_un.S_un_b.s_b1;
    BYTE b2 = addr.S_un.S_un_b.s_b2;
    if (b1 == 10) return true;
    if (b1 == 172 && b2 >= 16 && b2 <= 31) return true;
    if (b1 == 192 && b2 == 168) return true;
    return false;
}

} // namespace

std::vector<CheckResult> DiagnosticsEngine::RunTraceroute(const std::wstring& sourceIp, const std::wstring& target) {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"Traceroute to " + target + (sourceIp.empty() ? L"" : L" (from " + sourceIp + L")");
    r.category = DiagCategory::Network;

    std::wstring cmd = L"tracert -d -w 800 -h 14";
    if (!sourceIp.empty()) cmd += L" -S " + sourceIp;
    cmd += L" " + target;

    std::wstring output = RunCommandCaptureOutput(cmd, 60000);

    // Parse hop lines: "  N    <rtt> ms   <rtt> ms   <rtt> ms   <ip>" or
    // "  N     *        *        *     Request timed out." (with -d, no
    // hostnames appear, so any IPv4-looking token on the line is the hop).
    struct Hop { int number; std::wstring ip; bool timedOut; };
    std::vector<Hop> hops;
    std::wstringstream ss(output);
    std::wstring line;
    while (std::getline(ss, line)) {
        std::wstringstream ls(line);
        int hopNum = 0;
        if (!(ls >> hopNum)) continue; // not a hop line (banner/blank/summary text)

        std::wstring token, foundIp;
        while (ls >> token) {
            if (IsIPv4Address(token)) foundIp = token;
        }
        hops.push_back({ hopNum, foundIp, foundIp.empty() });
    }

    if (hops.empty()) {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.details = L"Could not run or parse tracert output. Raw output: " + output.substr(0, 300);
        out.push_back(r);
        return out;
    }

    std::wstring details;
    bool reachedTarget = false;
    bool anyCgnat = false;
    int timeoutCount = 0;
    for (auto& h : hops) {
        details += L"Hop " + std::to_wstring(h.number) + L": ";
        if (h.timedOut) {
            details += L"no response";
            ++timeoutCount;
        } else {
            details += h.ip;
            if (IsCgnatAddress(h.ip)) {
                details += L" [CARRIER-GRADE NAT - shared ISP address, RFC 6598]";
                anyCgnat = true;
            } else if (IsPrivateAddress(h.ip)) {
                details += L" [private/local address]";
            }
            if (h.ip == target) reachedTarget = true;
        }
        details += L"; ";
    }
    r.details = details;

    if (reachedTarget) {
        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
        if (anyCgnat) {
            r.recommendation = L"Your path passes through carrier-grade NAT (CGNAT) - very common for "
                                L"cable/mobile ISPs, and not itself a problem. It does mean port "
                                L"forwarding or hosting a server at home won't work without asking your "
                                L"ISP for a public/static IP, or using a tunneling service that provides "
                                L"one.";
        }
    } else if (timeoutCount == (int)hops.size()) {
        r.status = DiagStatus::Fail;
        r.severity = Severity::High;
        r.recommendation = L"Every hop timed out - the path may be blocked entirely, or this adapter "
                            L"has no working route out at all.";
    } else {
        r.status = DiagStatus::Warning;
        r.severity = Severity::Medium;
        r.recommendation = L"The trace didn't reach the destination within the hop limit. Some networks "
                            L"intentionally block ICMP (timeouts alone don't necessarily mean anything is "
                            L"broken), but if this is unexpected, the last responding hop is roughly "
                            L"where the path stops working.";
    }
    out.push_back(r);
    return out;
}

std::vector<CheckResult> DiagnosticsEngine::CheckLocalDevices() {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"Local network devices (ARP/neighbor table)";
    r.category = DiagCategory::Network;

    PMIB_IPNET_TABLE2 table = nullptr;
    if (GetIpNetTable2(AF_INET, &table) != NO_ERROR || !table) {
        r.status = DiagStatus::Info;
        r.severity = Severity::Low;
        r.details = L"Could not read the ARP/neighbor table.";
        out.push_back(r);
        return out;
    }

    std::vector<std::wstring> entries;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        auto& row = table->Table[i];
        // Only entries Windows has actually resolved to a real MAC - skips
        // Unreachable/Incomplete/multicast noise so this stays a genuinely
        // useful "what's on my LAN right now" list rather than table dump.
        if (row.State != NlnsReachable && row.State != NlnsStale && row.State != NlnsPermanent) continue;
        if (row.PhysicalAddressLength != 6) continue;

        auto sin = reinterpret_cast<const sockaddr_in*>(&row.Address.Ipv4);
        char ipBuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf));
        std::wstring ip = AnsiToWide(ipBuf, CP_ACP);
        if (ip == L"0.0.0.0" || ip.rfind(L"224.", 0) == 0 || ip == L"255.255.255.255") continue;

        wchar_t mac[18];
        swprintf_s(mac, L"%02X-%02X-%02X-%02X-%02X-%02X",
                   row.PhysicalAddress[0], row.PhysicalAddress[1], row.PhysicalAddress[2],
                   row.PhysicalAddress[3], row.PhysicalAddress[4], row.PhysicalAddress[5]);

        std::wstring adapterName;
        for (auto& a : m_adapters) {
            if (a.ifIndex == row.InterfaceIndex) { adapterName = a.name; break; }
        }

        entries.push_back(ip + L" (" + std::wstring(mac) + L")" +
                           (row.IsRouter ? L" [router]" : L"") +
                           (adapterName.empty() ? L"" : L" on " + adapterName));
    }
    FreeMibTable(table);

    r.status = DiagStatus::Info;
    r.severity = Severity::Low;
    if (entries.empty()) {
        r.details = L"No resolved local devices found in the ARP/neighbor table yet - normal shortly "
                    L"after startup before any local traffic has occurred.";
    } else {
        std::wstring joined;
        for (size_t i = 0; i < entries.size() && i < 25; ++i) {
            joined += entries[i];
            if (i + 1 < entries.size() && i < 24) joined += L" | ";
        }
        r.details = std::to_wstring(entries.size()) + L" device(s) currently visible on your local "
                    L"network: " + joined;
    }
    out.push_back(r);
    return out;
}

std::vector<CheckResult> DiagnosticsEngine::CheckDnsCache() {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"DNS client cache";
    r.category = DiagCategory::Network;

    std::wstring output = RunCommandCaptureOutput(L"ipconfig /displaydns", 15000);

    // Count cached record blocks (each entry's first line looks like
    // "    example.com" followed by "----------" then its record fields) -
    // count the separator lines as a reliable proxy for entry count without
    // needing a full parser.
    size_t count = 0;
    size_t pos = 0;
    while ((pos = output.find(L"----------", pos)) != std::wstring::npos) {
        ++count;
        pos += 10;
    }

    r.status = DiagStatus::Info;
    r.severity = Severity::Low;
    r.details = std::to_wstring(count) + L" entries currently cached. Full details available via "
                L"'ipconfig /displaydns' in an elevated terminal (Help tab has a quick-launch button) "
                L"if you need to see exactly what's cached for a specific hostname before deciding "
                L"whether to flush it.";
    out.push_back(r);
    return out;
}

std::vector<CheckResult> DiagnosticsEngine::RunDhcpBroadcastListener(int listenSeconds) {
    std::vector<CheckResult> out;
    CheckResult r;
    r.name = L"DHCP broadcast listener (LAN-side, passive)";
    r.category = DiagCategory::Network;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        r.status = DiagStatus::Info;
        r.severity = Severity::Low;
        r.details = L"Could not create a UDP socket for passive listening.";
        out.push_back(r);
        return out;
    }

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(67); // DHCP server port - listening here means "what's answering as a server"
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(sock);
        r.status = DiagStatus::Info;
        r.severity = Severity::Low;
        r.details = L"Could not bind UDP port 67 to listen (error " + std::to_wstring(err) +
                    L") - likely already in use by Internet Connection Sharing or a VM/container's "
                    L"own DHCP server running on this machine, which is itself useful to know: "
                    L"something local is already acting as a DHCP server.";
        out.push_back(r);
        return out;
    }

    DWORD sockTimeoutMs = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&sockTimeoutMs), sizeof(sockTimeoutMs));

    std::map<std::wstring, int> serversSeen; // server IP -> message count
    auto start = std::chrono::steady_clock::now();
    std::vector<BYTE> buf(2048);

    while (std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - start).count() < listenSeconds) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0,
                          reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) continue; // timeout (expected, keeps the loop bounded) or a benign recv error

        // Minimal BOOTP/DHCP sanity check: op=2 means BOOTREPLY (a server's
        // response), which is the only direction we care about here.
        if (n < 240 || buf[0] != 2) continue;

        char ipBuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &from.sin_addr, ipBuf, sizeof(ipBuf));
        serversSeen[AnsiToWide(ipBuf, CP_ACP)]++;
    }
    closesocket(sock);

    if (serversSeen.empty()) {
        r.status = DiagStatus::Info;
        r.severity = Severity::Low;
        r.details = L"No DHCP server activity observed in " + std::to_wstring(listenSeconds) +
                    L" second(s). This is passive - it only sees traffic that happens to occur "
                    L"during the listen window. For a reliable result, trigger a lease request while "
                    L"it's running (e.g. Network tab -> \"Release & renew DHCP on this adapter\", on "
                    L"this machine or another device on the same network).";
    } else if (serversSeen.size() == 1) {
        r.status = DiagStatus::Pass;
        r.severity = Severity::Low;
        r.details = L"One DHCP server observed: " + serversSeen.begin()->first + L" (" +
                    std::to_wstring(serversSeen.begin()->second) +
                    L" message(s)). Consistent with a single legitimate DHCP server on this segment.";
    } else {
        r.status = DiagStatus::Warning;
        r.severity = Severity::High;
        std::wstring joined;
        for (auto& kv : serversSeen) {
            joined += kv.first + L" (" + std::to_wstring(kv.second) + L" msg), ";
        }
        r.details = std::to_wstring(serversSeen.size()) + L" DIFFERENT DHCP servers observed "
                    L"responding on this network segment: " + joined +
                    L"Multiple DHCP servers answering on the same LAN is the classic signature of "
                    L"either a misconfigured second router (someone plugged in a home router with "
                    L"DHCP still enabled) or a rogue/spoofed DHCP server.";
        r.recommendation = L"Check \"Local network devices\" (Network tab) to match each IP to a MAC "
                            L"address, and physically identify any unexpected router/device.";
    }
    out.push_back(r);
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
