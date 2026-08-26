// DiagnosticsEngine.h - performs all diagnostic checks.
// Everything here is LOCAL: adapter/registry/service/WMI/event-log queries and
// pings to the default gateway / configured DNS servers on the LAN. No check in
// this file requires or assumes internet access, matching the offline design goal.
#pragma once
#include "Common.h"
#include "WMIHelper.h"
#include <vector>
#include <string>

// Classifies what an adapter is actually doing on the network, so a
// multi-NIC machine (e.g. one adapter for internet, another for a direct
// point-to-point link to another PC, plus Hyper-V/WSL virtual switches)
// gets diagnosed per-role instead of every adapter being judged by the same
// "should have internet" yardstick.
enum class AdapterRole {
    Disconnected,          // media disconnected / cable unplugged / Wi-Fi off
    Virtual,               // Hyper-V/WSL/VPN virtual switch - not physical hardware
    GatewayScopeMismatch,  // APIPA (169.254.x.x) address but a routable default
                           // gateway is still configured on the adapter - invalid;
                           // 169.254.0.0/16 is link-local-only per RFC 3927 and
                           // must never have a usable default route attached to it
    DirectLink,            // APIPA address, no gateway configured - correct
                           // behavior for a direct cable link with no DHCP server
    Isolated,              // has a routable IP but no gateway/route at all
    Routed                 // normal IP + working default gateway
};

std::wstring AdapterRoleToString(AdapterRole r);

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

    AdapterRole role = AdapterRole::Disconnected;
    bool isVirtual = false;
    DWORD metric = 0;              // interface metric (lower = more preferred for the default route)
    DWORD mtu = 0;                  // interface MTU in bytes - a mismatch here is a real (if less
                                     // common) cause of specific-site failures over VPN/PPPoE links
    std::wstring gatewayNudState;  // Neighbor Unreachability Detection state for the
                                    // gateway's ARP entry: Reachable/Probe/Stale/
                                    // Unreachable/etc - "Probe" here is exactly the
                                    // signature of an adapter Windows hasn't
                                    // confirmed can actually reach its gateway.
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

    // Re-enumerates and classifies adapters (role/metric/NUD state) without
    // running the rest of RunNetworkChecks - used by the Topology tab so it
    // can refresh the diagram on its own schedule.
    std::vector<NetworkAdapterInfo> GetAdapterTopology();

    std::vector<NetworkAdapterInfo> GetAdapters() const { return m_adapters; }

    // Sends a single ping to a specific address. Used by the Topology tab's
    // "Reverify Gateway" button: a successful reply is exactly what makes
    // Windows' IP stack re-confirm a neighbor's reachability (per the
    // standard Neighbor Unreachability Detection state machine), moving it
    // from Stale/Probe back toward Reachable - this is a completely
    // ordinary mechanism, not a special API.
    bool PingAddress(const std::wstring& ip) { return PingHost(ip, 2000); }

    // Passively listens for DHCP server traffic (BOOTREPLY messages) already
    // occurring on the local network segment, for 'listenSeconds'. This is
    // deliberately NOT a raw-socket/packet-injection design: it's an
    // ordinary bound UDP socket (SOCK_DGRAM) on port 67, doing nothing a
    // normal application couldn't already do - no promiscuous mode, no
    // custom packet construction, no third-party capture library. It can
    // only see what's already on the wire, so absence of a result doesn't
    // prove absence of a server - trigger real traffic during the listen
    // window (e.g. "Release & renew DHCP on this adapter") for a reliable
    // read. Seeing MORE THAN ONE distinct server is the actual point: that's
    // the standard signature of a rogue/duplicate DHCP server on the LAN.
    // On-demand only - not part of the automatic scans.
    static std::vector<CheckResult> RunDhcpBroadcastListener(int listenSeconds = 12);

    // Runs tracert toward 'target' to reveal the path BEYOND the local
    // default gateway - switches/hubs are invisible (they're Layer 2, no IP
    // hop), but additional routers, modems, and ISP equipment show up here,
    // including detecting carrier-grade NAT (CGNAT, RFC 6598, 100.64.0.0/10)
    // which is otherwise completely invisible from the LAN side. Optionally
    // forces the trace out through a specific local adapter (sourceIp)
    // rather than letting Windows pick the route. On-demand only (can take
    // up to ~30-60 seconds) - not part of the automatic scans.
    static std::vector<CheckResult> RunTraceroute(const std::wstring& sourceIp = L"",
                                                    const std::wstring& target = L"1.1.1.1");

    // "What's actually on my network right now" - a device inventory from
    // the ARP/neighbor table, with each entry's MAC address and which local
    // adapter it was seen on. Instance method (needs m_adapters for the
    // ifIndex -> friendly-name lookup).
    std::vector<CheckResult> CheckLocalDevices();

    // Shows what's currently cached by the DNS client - useful to see BEFORE
    // deciding to flush it (a stale/wrong cached entry explains a symptom
    // far more precisely than "flush it and hope").
    static std::vector<CheckResult> CheckDnsCache();

private:
    std::vector<NetworkAdapterInfo> m_adapters;
    WMIHelper m_wmi;
    bool m_winsockReady = false;

    bool EnumerateAdapters();

    // Confirms, via a direct API query rather than an inference from metric
    // numbers, which adapter Windows will actually use for general internet
    // traffic - and separately, whether anything (a VPN client, leftover
    // config) has injected a manual route that silently overrides normal
    // adapter selection for a specific destination. Both need m_adapters
    // for ifIndex -> friendly-name lookup, so these are instance methods
    // rather than static like the checks below.
    std::vector<CheckResult> CheckActiveRouting();
    std::vector<CheckResult> CheckStaticRoutes();
    void ClassifyAdapters(); // fills in role/metric/gatewayNudState/isVirtual for m_adapters
    bool PingHost(const std::wstring& hostOrIp, DWORD timeoutMs = 1500);
    bool CanResolve(const std::wstring& hostname);
    static bool IsVirtualAdapterDescription(const std::wstring& desc);
    static DWORD GetInterfaceMetric(DWORD ifIndex);
    static DWORD GetInterfaceMtu(DWORD ifIndex);
    static std::wstring GetGatewayNudState(DWORD ifIndex, const std::wstring& gatewayIp);

    // These three specifically target "the network/internet looks fine but
    // browsing still doesn't work" - each is a classic cause that a plain
    // adapter/gateway/DNS check completely misses.
    static std::vector<CheckResult> CheckHostsFile();     // hijacked/stale entries in the hosts file
    static std::vector<CheckResult> CheckProxySettings(); // WinINet + WinHTTP proxy misconfiguration
    static std::vector<CheckResult> CheckTimeSync();      // wrong system clock breaks TLS/HTTPS silently
    static std::vector<CheckResult> CheckSystemRestore();  // is there a whole-system undo button available?
    static std::vector<CheckResult> CheckAutostartEntries(); // registry Run/RunOnce - explains "it keeps coming back"
};
