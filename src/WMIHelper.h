// WMIHelper.h - thin wrapper around local WMI (COM) queries.
// WMI is a local management service (root\CIMV2 etc.) - no network/internet access
// is required or performed here.
#pragma once
#include <string>
#include <vector>
#include <map>

class WMIHelper {
public:
    WMIHelper();
    ~WMIHelper();

    // Returns false if COM/WMI could not be initialized (caller should skip hardware checks).
    bool Initialize();
    void Shutdown();
    bool IsReady() const { return m_ready; }

    // Runs a WQL query against ROOT\CIMV2 and returns one map of property->value per row.
    // Only string-representable scalar/array properties are captured.
    std::vector<std::map<std::wstring, std::wstring>> Query(const std::wstring& wql);

private:
    bool m_comInitialized = false;
    bool m_ready = false;
    void* m_pLoc = nullptr; // IWbemLocator*
    void* m_pSvc = nullptr; // IWbemServices*
};
