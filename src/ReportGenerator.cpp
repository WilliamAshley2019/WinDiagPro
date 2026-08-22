#include "ReportGenerator.h"
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")

static std::wstring EscapeHtml(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

// Escapes a string for use inside a JSON string literal.
static std::wstring EscapeJson(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf_s(buf, L"\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Pipe-escapes a string so it's safe inside a Markdown table cell (newlines
// would otherwise break the row; '|' would otherwise be read as a new column).
static std::wstring EscapeMarkdownCell(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'|') out += L"\\|";
        else if (c == L'\n') out += L"<br>";
        else if (c == L'\r') { /* skip */ }
        else out += c;
    }
    return out;
}

std::wstring ReportGenerator::BuildText(const std::vector<CheckResult>& results,
                                          const std::vector<Diagnosis>& diagnoses) {
    std::wstring out;
    out += L"====================================================\r\n";
    out += L"          WinDiagPro Diagnostic Report\r\n";
    out += L"====================================================\r\n";
    out += L"Generated: " + CurrentTimestamp() + L"\r\n\r\n";

    int pass = 0, fail = 0, warn = 0, info = 0;
    for (auto& r : results) {
        switch (r.status) {
            case DiagStatus::Pass: pass++; break;
            case DiagStatus::Fail: fail++; break;
            case DiagStatus::Warning: warn++; break;
            case DiagStatus::Info: info++; break;
        }
    }
    out += L"SUMMARY: " + std::to_wstring(pass) + L" pass, " + std::to_wstring(fail) +
           L" fail, " + std::to_wstring(warn) + L" warning, " + std::to_wstring(info) + L" info\r\n\r\n";

    if (!diagnoses.empty()) {
        out += L"----------------------------------------------------\r\n";
        out += L"LIKELY ROOT CAUSE(S) AND RECOMMENDED ACTIONS\r\n";
        out += L"----------------------------------------------------\r\n";
        for (auto& d : diagnoses) {
            out += L"[" + SeverityToString(d.severity) + L"] " + d.title + L"\r\n";
            out += L"  " + d.explanation + L"\r\n";
            if (!d.suggestedRepairIds.empty()) {
                out += L"  Suggested repairs: ";
                for (size_t i = 0; i < d.suggestedRepairIds.size(); ++i) {
                    out += d.suggestedRepairIds[i];
                    if (i + 1 < d.suggestedRepairIds.size()) out += L", ";
                }
                out += L"\r\n";
            }
            out += L"\r\n";
        }
    }

    out += L"----------------------------------------------------\r\n";
    out += L"DETAILED RESULTS\r\n";
    out += L"----------------------------------------------------\r\n";
    for (auto& r : results) {
        out += L"[" + StatusToString(r.status) + L"] (" + CategoryToString(r.category) + L"/" +
               SeverityToString(r.severity) + L") " + r.name + L"\r\n";
        out += L"    " + r.details + L"\r\n";
        if (!r.recommendation.empty()) out += L"    -> " + r.recommendation + L"\r\n";
        out += L"\r\n";
    }
    return out;
}

std::wstring ReportGenerator::BuildHtml(const std::vector<CheckResult>& results,
                                          const std::vector<Diagnosis>& diagnoses) {
    std::wstring html;
    html += L"<!DOCTYPE html><html><head><meta charset='utf-8'><title>WinDiagPro Report</title>";
    html += L"<style>"
            L"body{font-family:Segoe UI,Arial,sans-serif;margin:24px;background:#f6f7f9;color:#1c1c1c}"
            L"h1{font-size:20px} h2{font-size:16px;margin-top:28px}"
            L".summary{margin-bottom:16px}"
            L".diag{border-left:4px solid #888;background:#fff;padding:10px 14px;margin:10px 0;border-radius:4px}"
            L".sev-Critical{border-color:#c0392b} .sev-High{border-color:#e67e22}"
            L".sev-Medium{border-color:#f1c40f} .sev-Low{border-color:#27ae60}"
            L"table{border-collapse:collapse;width:100%;background:#fff}"
            L"td,th{border:1px solid #ddd;padding:6px 10px;font-size:13px;text-align:left;vertical-align:top}"
            L"th{background:#eee}"
            L".Pass{color:#1e7e34;font-weight:600}.Fail{color:#c0392b;font-weight:600}"
            L".Warning{color:#8a6d00;font-weight:600}.Info{color:#555}"
            L"</style></head><body>";

    html += L"<h1>WinDiagPro Diagnostic Report</h1>";
    html += L"<div class='summary'>Generated: " + EscapeHtml(CurrentTimestamp()) + L"</div>";

    if (!diagnoses.empty()) {
        html += L"<h2>Likely root cause(s) and recommended actions</h2>";
        for (auto& d : diagnoses) {
            html += L"<div class='diag sev-" + SeverityToString(d.severity) + L"'>";
            html += L"<b>[" + SeverityToString(d.severity) + L"] " + EscapeHtml(d.title) + L"</b><br/>";
            html += EscapeHtml(d.explanation);
            if (!d.suggestedRepairIds.empty()) {
                html += L"<br/><i>Suggested repairs: ";
                for (size_t i = 0; i < d.suggestedRepairIds.size(); ++i) {
                    html += EscapeHtml(d.suggestedRepairIds[i]);
                    if (i + 1 < d.suggestedRepairIds.size()) html += L", ";
                }
                html += L"</i>";
            }
            html += L"</div>";
        }
    }

    html += L"<h2>Detailed results</h2><table><tr><th>Status</th><th>Category</th><th>Severity</th>"
            L"<th>Check</th><th>Details</th><th>Recommendation</th></tr>";
    for (auto& r : results) {
        html += L"<tr><td class='" + StatusToString(r.status) + L"'>" + StatusToString(r.status) + L"</td>";
        html += L"<td>" + CategoryToString(r.category) + L"</td>";
        html += L"<td>" + SeverityToString(r.severity) + L"</td>";
        html += L"<td>" + EscapeHtml(r.name) + L"</td>";
        html += L"<td>" + EscapeHtml(r.details) + L"</td>";
        html += L"<td>" + EscapeHtml(r.recommendation) + L"</td></tr>";
    }
    html += L"</table></body></html>";
    return html;
}

std::wstring ReportGenerator::BuildMarkdown(const std::vector<CheckResult>& results,
                                              const std::vector<Diagnosis>& diagnoses) {
    std::wstring md;
    md += L"# WinDiagPro Diagnostic Report\r\n\r\n";
    md += L"_Generated: " + CurrentTimestamp() + L"_\r\n\r\n";

    int pass = 0, fail = 0, warn = 0, info = 0;
    for (auto& r : results) {
        switch (r.status) {
            case DiagStatus::Pass: pass++; break;
            case DiagStatus::Fail: fail++; break;
            case DiagStatus::Warning: warn++; break;
            case DiagStatus::Info: info++; break;
        }
    }
    md += L"**Summary:** " + std::to_wstring(pass) + L" pass, " + std::to_wstring(fail) +
          L" fail, " + std::to_wstring(warn) + L" warning, " + std::to_wstring(info) + L" info\r\n\r\n";

    if (!diagnoses.empty()) {
        md += L"## Likely root cause(s) and recommended actions\r\n\r\n";
        for (auto& d : diagnoses) {
            md += L"### [" + SeverityToString(d.severity) + L"] " + d.title + L"\r\n\r\n";
            md += d.explanation + L"\r\n\r\n";
            if (!d.suggestedRepairIds.empty()) {
                md += L"Suggested repairs: ";
                for (size_t i = 0; i < d.suggestedRepairIds.size(); ++i) {
                    md += L"`" + d.suggestedRepairIds[i] + L"`";
                    if (i + 1 < d.suggestedRepairIds.size()) md += L", ";
                }
                md += L"\r\n\r\n";
            }
        }
    }

    md += L"## Detailed results\r\n\r\n";
    md += L"| Status | Category | Severity | Check | Details | Recommendation |\r\n";
    md += L"|---|---|---|---|---|---|\r\n";
    for (auto& r : results) {
        md += L"| " + StatusToString(r.status) + L" | " + CategoryToString(r.category) + L" | " +
              SeverityToString(r.severity) + L" | " + EscapeMarkdownCell(r.name) + L" | " +
              EscapeMarkdownCell(r.details) + L" | " + EscapeMarkdownCell(r.recommendation) + L" |\r\n";
    }
    return md;
}

std::wstring ReportGenerator::BuildJson(const std::vector<CheckResult>& results,
                                          const std::vector<Diagnosis>& diagnoses) {
    int pass = 0, fail = 0, warn = 0, info = 0;
    for (auto& r : results) {
        switch (r.status) {
            case DiagStatus::Pass: pass++; break;
            case DiagStatus::Fail: fail++; break;
            case DiagStatus::Warning: warn++; break;
            case DiagStatus::Info: info++; break;
        }
    }

    std::wstring j;
    j += L"{\r\n";
    j += L"  \"generated\": \"" + EscapeJson(CurrentTimestamp()) + L"\",\r\n";
    j += L"  \"summary\": { \"pass\": " + std::to_wstring(pass) + L", \"fail\": " + std::to_wstring(fail) +
         L", \"warning\": " + std::to_wstring(warn) + L", \"info\": " + std::to_wstring(info) + L" },\r\n";

    j += L"  \"diagnoses\": [\r\n";
    for (size_t i = 0; i < diagnoses.size(); ++i) {
        auto& d = diagnoses[i];
        j += L"    {\r\n";
        j += L"      \"title\": \"" + EscapeJson(d.title) + L"\",\r\n";
        j += L"      \"severity\": \"" + SeverityToString(d.severity) + L"\",\r\n";
        j += L"      \"explanation\": \"" + EscapeJson(d.explanation) + L"\",\r\n";
        j += L"      \"suggestedRepairIds\": [";
        for (size_t k = 0; k < d.suggestedRepairIds.size(); ++k) {
            j += L"\"" + EscapeJson(d.suggestedRepairIds[k]) + L"\"";
            if (k + 1 < d.suggestedRepairIds.size()) j += L", ";
        }
        j += L"]\r\n    }";
        if (i + 1 < diagnoses.size()) j += L",";
        j += L"\r\n";
    }
    j += L"  ],\r\n";

    j += L"  \"results\": [\r\n";
    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        j += L"    {\r\n";
        j += L"      \"status\": \"" + StatusToString(r.status) + L"\",\r\n";
        j += L"      \"category\": \"" + CategoryToString(r.category) + L"\",\r\n";
        j += L"      \"severity\": \"" + SeverityToString(r.severity) + L"\",\r\n";
        j += L"      \"name\": \"" + EscapeJson(r.name) + L"\",\r\n";
        j += L"      \"details\": \"" + EscapeJson(r.details) + L"\",\r\n";
        j += L"      \"recommendation\": \"" + EscapeJson(r.recommendation) + L"\"\r\n";
        j += L"    }";
        if (i + 1 < results.size()) j += L",";
        j += L"\r\n";
    }
    j += L"  ]\r\n";
    j += L"}\r\n";
    return j;
}

std::wstring ReportGenerator::FormatResultLineTsv(const CheckResult& r) {
    std::wstring line;
    line += StatusToString(r.status) + L"\t";
    line += CategoryToString(r.category) + L"\t";
    line += SeverityToString(r.severity) + L"\t";
    line += r.name + L"\t";
    line += r.details + L"\t";
    line += r.recommendation;
    return line;
}

// Writes raw UTF-16LE bytes (with BOM) directly via the Win32 file API - avoids
// std::wofstream locale/codecvt pitfalls with wide characters on Windows.
// Used for .txt so Notepad/Notepad++ always render it correctly regardless
// of the user's system codepage.
static std::wstring WriteRawFileUtf16(const std::wstring& content, const std::wstring& folder, const std::wstring& ext) {
    std::wstring dir = folder;
    if (dir.empty()) {
        wchar_t docsPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docsPath))) {
            dir = docsPath;
        } else {
            dir = L".";
        }
    }
    std::wstring path = dir + L"\\WinDiagPro_Report_" + CurrentTimestampForFilename() + ext;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";

    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(h, &bom, sizeof(bom), &written, nullptr);
    WriteFile(h, content.c_str(), (DWORD)(content.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    return path;
}

// Writes UTF-8 bytes with no BOM - used for .html (matches its declared
// <meta charset='utf-8'>), .md, and .json, since browsers/Markdown viewers/
// JSON parsers (including AI tooling ingesting the file) universally expect
// UTF-8 and a stray BOM can trip up strict JSON parsers.
static std::wstring WriteRawFileUtf8(const std::wstring& content, const std::wstring& folder, const std::wstring& ext) {
    std::wstring dir = folder;
    if (dir.empty()) {
        wchar_t docsPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docsPath))) {
            dir = docsPath;
        } else {
            dir = L".";
        }
    }
    std::wstring path = dir + L"\\WinDiagPro_Report_" + CurrentTimestampForFilename() + ext;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";

    std::string utf8 = WideToUtf8(content);
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(h);
    return path;
}

std::wstring ReportGenerator::SaveText(const std::wstring& text, const std::wstring& folder) {
    return WriteRawFileUtf16(text, folder, L".txt");
}

std::wstring ReportGenerator::SaveHtml(const std::wstring& html, const std::wstring& folder) {
    return WriteRawFileUtf8(html, folder, L".html");
}

std::wstring ReportGenerator::SaveMarkdown(const std::wstring& md, const std::wstring& folder) {
    return WriteRawFileUtf8(md, folder, L".md");
}

std::wstring ReportGenerator::SaveJson(const std::wstring& json, const std::wstring& folder) {
    return WriteRawFileUtf8(json, folder, L".json");
}
