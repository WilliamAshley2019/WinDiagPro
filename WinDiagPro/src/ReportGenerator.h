// ReportGenerator.h - builds plain-text, HTML, Markdown, and JSON diagnostic
// reports, and saves them to disk. Purely local file I/O - nothing is ever
// uploaded anywhere. Markdown/JSON exist specifically so a report can be
// pasted or attached to an AI assistant or ticket system in a format that
// parses cleanly, instead of only the human-oriented text/HTML forms.
#pragma once
#include "Common.h"
#include "RulesEngine.h"
#include <vector>

class ReportGenerator {
public:
    static std::wstring BuildText(const std::vector<CheckResult>& results,
                                   const std::vector<Diagnosis>& diagnoses);
    static std::wstring BuildHtml(const std::vector<CheckResult>& results,
                                   const std::vector<Diagnosis>& diagnoses);
    static std::wstring BuildMarkdown(const std::vector<CheckResult>& results,
                                       const std::vector<Diagnosis>& diagnoses);
    static std::wstring BuildJson(const std::vector<CheckResult>& results,
                                   const std::vector<Diagnosis>& diagnoses);

    // Returns the full path written, or empty string on failure.
    static std::wstring SaveText(const std::wstring& text, const std::wstring& folder = L"");
    static std::wstring SaveHtml(const std::wstring& html, const std::wstring& folder = L"");
    static std::wstring SaveMarkdown(const std::wstring& md, const std::wstring& folder = L"");
    static std::wstring SaveJson(const std::wstring& json, const std::wstring& folder = L"");

    // Formats a single result as one tab-separated line (Status, Category,
    // Severity, Check, Details, Recommendation) - used for clipboard copy of
    // list view rows so pasted text lines up nicely in Excel/Notepad/etc.
    static std::wstring FormatResultLineTsv(const CheckResult& r);
};
