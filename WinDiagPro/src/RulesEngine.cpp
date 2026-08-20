#include "RulesEngine.h"
#include <algorithm>

static bool NameContains(const CheckResult& r, const wchar_t* needle) {
    return r.name.find(needle) != std::wstring::npos;
}

static const CheckResult* Find(const std::vector<CheckResult>& results, const wchar_t* needle) {
    for (auto& r : results) if (NameContains(r, needle)) return &r;
    return nullptr;
}

static int CountStatus(const std::vector<CheckResult>& results, const wchar_t* needle, DiagStatus status) {
    int n = 0;
    for (auto& r : results) if (NameContains(r, needle) && r.status == status) n++;
    return n;
}

std::vector<Diagnosis> RulesEngine::Analyze(const std::vector<CheckResult>& results) {
    std::vector<Diagnosis> out;

    const CheckResult* adapterDisconnected = nullptr;
    for (auto& r : results) {
        if (r.name.rfind(L"Adapter: ", 0) == 0 && r.status != DiagStatus::Pass) {
            adapterDisconnected = &r;
            break;
        }
    }
    const CheckResult* winsock = Find(results, L"Winsock catalog");
    int gatewayFails = CountStatus(results, L"Gateway reachability", DiagStatus::Fail);
    int gatewayTotal = 0;
    for (auto& r : results) if (NameContains(r, L"Gateway reachability")) gatewayTotal++;
    int dnsServerFails = CountStatus(results, L"DNS server", DiagStatus::Warning) +
                          CountStatus(results, L"DNS server", DiagStatus::Fail);
    int dnsServerTotal = 0;
    for (auto& r : results) if (NameContains(r, L"DNS server")) dnsServerTotal++;

    // 1) No adapter connected at all -> physical/link layer problem, everything downstream is noise.
    if (adapterDisconnected && gatewayTotal == 0) {
        Diagnosis d;
        d.title = L"No active network connection";
        d.severity = Severity::High;
        d.explanation = L"No network adapter currently has an active, connected link, so DNS/gateway/"
                         L"internet checks could not run. This is very likely a cable, Wi-Fi, or "
                         L"adapter-enable problem rather than a configuration problem.";
        d.suggestedRepairIds = {}; // no software fix applies here
        out.push_back(d);
        return out; // downstream diagnoses would just be restating the same root cause
    }

    // 2) Winsock catalog is empty/corrupt -> explains simultaneous DNS+gateway+internet failures.
    if (winsock && winsock->status == DiagStatus::Fail && gatewayFails > 0) {
        Diagnosis d;
        d.title = L"Corrupt Winsock configuration";
        d.severity = Severity::Critical;
        d.explanation = L"The Winsock protocol catalog looks corrupt or empty AND network reachability "
                         L"checks are failing at the same time. A corrupt Winsock catalog can cause "
                         L"exactly this pattern (everything network-related fails at once, even though "
                         L"the adapter itself is connected).";
        d.suggestedRepairIds = { L"reset_winsock", L"reset_tcpip" };
        out.push_back(d);
    }

    // 3) Gateway unreachable on a connected adapter -> local LAN/router problem, not a DNS problem.
    if (gatewayTotal > 0 && gatewayFails == gatewayTotal) {
        Diagnosis d;
        d.title = L"Router/gateway unreachable";
        d.severity = Severity::High;
        d.explanation = L"Every connected adapter reports its default gateway as unreachable. DNS "
                         L"failures seen elsewhere in this report are a side effect of this, not a "
                         L"separate DNS problem - fixing DNS settings will not help until the gateway "
                         L"itself responds.";
        d.suggestedRepairIds = { L"restart_nla_svc" };
        out.push_back(d);
    }
    // 4) Gateway is fine but DNS servers are not responding -> genuine DNS-layer problem.
    else if (dnsServerTotal > 0 && dnsServerFails == dnsServerTotal && gatewayFails == 0) {
        Diagnosis d;
        d.title = L"DNS servers unreachable (gateway is fine)";
        d.severity = Severity::Medium;
        d.explanation = L"The gateway responds normally, but none of the configured DNS servers do. "
                         L"This points specifically at DNS configuration or the DNS Client service, "
                         L"not general connectivity.";
        d.suggestedRepairIds = { L"restart_dns_svc", L"flush_dns" };
        out.push_back(d);
    }

    // 5) DHCP-enabled adapter with no IP -> DHCP lease problem.
    for (auto& r : results) {
        if (r.name.rfind(L"Adapter: ", 0) == 0 && r.status == DiagStatus::Fail &&
            r.recommendation.find(L"Renew the DHCP lease") != std::wstring::npos) {
            Diagnosis d;
            d.title = L"DHCP lease failure";
            d.severity = Severity::High;
            d.explanation = L"An adapter is connected and configured for DHCP but never received an IP "
                             L"address lease. This usually means no DHCP server answered - check the "
                             L"router's DHCP service, or renew the lease if a server is expected to be "
                             L"available.";
            d.suggestedRepairIds = { L"renew_dhcp", L"restart_dhcp_svc" };
            out.push_back(d);
            break;
        }
    }

    // If nothing structured matched but there are still individual failures, surface a generic note
    // rather than staying silent.
    if (out.empty()) {
        int failCount = 0;
        for (auto& r : results) if (r.status == DiagStatus::Fail) failCount++;
        if (failCount > 0) {
            Diagnosis d;
            d.title = L"Multiple isolated issues found";
            d.severity = Severity::Medium;
            d.explanation = L"No single root cause pattern was detected, but " + std::to_wstring(failCount) +
                             L" individual check(s) failed. Review the detailed results below.";
            out.push_back(d);
        }
    }

    std::sort(out.begin(), out.end(), [](const Diagnosis& a, const Diagnosis& b) {
        return (int)a.severity > (int)b.severity;
    });
    return out;
}
