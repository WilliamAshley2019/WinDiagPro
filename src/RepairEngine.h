// RepairEngine.h - remediation actions. All actions operate purely locally
// (netsh/ipconfig/sc against the local machine) - nothing here talks to the
// internet, matching the offline design goal.
#pragma once
#include "Common.h"
#include <vector>

class RepairEngine {
public:
    // Returns the full catalog of repair actions available in this build.
    static std::vector<RepairAction> GetCatalog();

    // Convenience direct calls (also used by the catalog above).
    static bool FlushDns(std::wstring& log);
    static bool ResetWinsock(std::wstring& log);
    static bool ResetTcpIp(std::wstring& log);
    static bool RenewDhcpAll(std::wstring& log);
    static bool RestartService(const std::wstring& serviceName, std::wstring& log);

    // Full Windows Update component reset: stops wuauserv/BITS/CryptSvc/msiserver,
    // renames %windir%\SoftwareDistribution and %windir%\System32\catroot2 out of
    // the way (never deletes - Windows rebuilds them automatically, and the old
    // folders are left on disk with a timestamped .bak suffix so this is fully
    // reversible), then restarts the services. This is the standard community
    // fix for a Windows Update service that runs but never actually installs
    // updates - a plain service restart alone usually isn't enough for that.
    static bool FullWindowsUpdateReset(std::wstring& log);

    // Releases and renews the DHCP lease on ONE specific adapter (by its
    // friendly name, e.g. "Ethernet 2") rather than every adapter on the
    // machine. Precise, targeted fix for a multi-NIC setup where only one
    // adapter's lease is actually broken - matters a lot when another
    // adapter's lease is healthy and shouldn't be disturbed.
    static bool ReleaseRenewAdapter(const std::wstring& adapterName, std::wstring& log);

    // Exports the driver package (INF + associated files, already present in
    // the local driver store - nothing is downloaded) for every currently
    // installed network adapter, via the built-in pnputil tool. A safety net
    // to run before any invasive network repair: if a reset ever leaves an
    // adapter without its driver, you have an offline copy ready to reinstall
    // without needing internet access to fetch it again.
    // outFolder receives the destination folder actually used.
    static bool BackupNetworkDrivers(std::wstring& log, std::wstring& outFolder);

    // Captures the current network configuration (ipconfig /all, route print,
    // netsh interface ip show config) to a timestamped snapshot file, purely
    // for before/after comparison - matches the "take a snapshot before you
    // touch anything" discipline that makes network troubleshooting
    // reversible instead of a one-way trip. Returns the saved file path, or
    // empty on failure.
    static std::wstring ExportNetworkConfigSnapshot(std::wstring& log, const std::wstring& folder = L"");

    // Resets the WinHTTP proxy (used by Windows Update and many services) to
    // "direct access, no proxy" - fixes the common "leftover corporate/VPN
    // proxy still configured" cause of Windows Update / background service
    // failures that a browser proxy setting change alone won't touch.
    static bool ResetWinHttpProxy(std::wstring& log);

    // Creates a Windows System Restore point via PowerShell's Checkpoint-Computer -
    // a whole-system undo button (registry, installed drivers, system files),
    // not just a network settings rollback. Requires System Protection to be
    // enabled for at least one drive; Windows normally allows only one of
    // these every 24 hours by default.
    static bool CreateRestorePoint(std::wstring& log);

    // Temporarily points one specific adapter's DNS at a public resolver
    // (Cloudflare's 1.1.1.1 / 1.0.0.1) - a targeted test/fix for "the
    // configured DNS server itself is down or unreachable" without touching
    // any other adapter or the rest of the network config. Pair with
    // RestoreAdapterDnsToDhcp to undo it.
    static bool SetAdapterDnsToPublic(const std::wstring& adapterName, std::wstring& log);
    static bool RestoreAdapterDnsToDhcp(const std::wstring& adapterName, std::wstring& log);
};
