// RulesEngine.h - combines individual CheckResults into a small number of
// higher-level diagnoses with a ranked, actionable recommendation - the part
// the original MSDT-style troubleshooters did poorly ("if DNS fails, flush DNS"
// regardless of *why* DNS failed). This looks at the whole picture at once.
#pragma once
#include "Common.h"
#include <vector>

struct Diagnosis {
    std::wstring title;
    Severity severity;
    std::wstring explanation;
    std::vector<std::wstring> suggestedRepairIds; // matches RepairAction::id values
};

class RulesEngine {
public:
    // Looks at every CheckResult produced this run and derives 0+ diagnoses,
    // ordered from most to least likely/severe.
    static std::vector<Diagnosis> Analyze(const std::vector<CheckResult>& results);
};
