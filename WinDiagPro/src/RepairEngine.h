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
};
