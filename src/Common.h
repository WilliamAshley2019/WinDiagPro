// Common.h - shared types and utility helpers used across WinDiagPro
// No external/third-party dependencies - Windows SDK only. Fully offline.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <functional>

enum class DiagCategory {
    Network,
    System,
    Hardware,
    Security,
    Performance
};

enum class DiagStatus {
    Pass,
    Fail,
    Warning,
    Info
};

enum class Severity {
    Low,
    Medium,
    High,
    Critical
};

struct CheckResult {
    std::wstring name;
    DiagCategory category = DiagCategory::System;
    DiagStatus status = DiagStatus::Info;
    Severity severity = Severity::Low;
    std::wstring details;
    std::wstring recommendation;
};

// A repair/remediation action the user (or the rules engine) can trigger.
// execute() returns true on success and writes human-readable log lines to 'log'.
struct RepairAction {
    std::wstring id;
    std::wstring name;
    std::wstring description;
    bool requiresReboot = false;
    std::function<bool(std::wstring& log)> execute;
};

// ---- String / formatting helpers ----
std::wstring StatusToString(DiagStatus s);
std::wstring SeverityToString(Severity s);
std::wstring CategoryToString(DiagCategory c);
std::wstring CurrentTimestamp();          // "YYYY-MM-DD HH:MM:SS"
std::wstring CurrentTimestampForFilename();// "YYYY-MM-DD_HH-MM-SS"
std::wstring AnsiToWide(const std::string& s, UINT codepage = CP_OEMCP);
std::string  WideToUtf8(const std::wstring& s);

// ---- Process helpers ----
// Runs a command line via cmd.exe /c, captures combined stdout+stderr, waits up to timeoutMs.
// Returns the captured text (best-effort, console code page decoded).
std::wstring RunCommandCaptureOutput(const std::wstring& commandLine, DWORD timeoutMs = 60000);

// True if the current process token is elevated (running as Administrator).
bool IsElevated();

// ---- Clipboard ----
// Copies UTF-16 text to the system clipboard (CF_UNICODETEXT) so it can be
// pasted into Notepad, a browser, an issue tracker, etc. Returns false if the
// clipboard could not be opened/written.
bool CopyTextToClipboard(HWND owner, const std::wstring& text);
