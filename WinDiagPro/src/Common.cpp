#include "Common.h"
#include <sstream>
#include <iomanip>
#include <cstring>

std::wstring StatusToString(DiagStatus s) {
    switch (s) {
        case DiagStatus::Pass:    return L"PASS";
        case DiagStatus::Fail:    return L"FAIL";
        case DiagStatus::Warning: return L"WARN";
        case DiagStatus::Info:    return L"INFO";
    }
    return L"?";
}

std::wstring SeverityToString(Severity s) {
    switch (s) {
        case Severity::Low:      return L"Low";
        case Severity::Medium:   return L"Medium";
        case Severity::High:     return L"High";
        case Severity::Critical: return L"Critical";
    }
    return L"?";
}

std::wstring CategoryToString(DiagCategory c) {
    switch (c) {
        case DiagCategory::Network:     return L"Network";
        case DiagCategory::System:      return L"System";
        case DiagCategory::Hardware:    return L"Hardware";
        case DiagCategory::Security:    return L"Security";
        case DiagCategory::Performance: return L"Performance";
    }
    return L"?";
}

std::wstring CurrentTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring CurrentTimestampForFilename() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d_%02d-%02d-%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring AnsiToWide(const std::string& s, UINT codepage) {
    if (s.empty()) return L"";
    int needed = MultiByteToWideChar(codepage, 0, s.data(), (int)s.size(), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(needed, L'\0');
    MultiByteToWideChar(codepage, 0, s.data(), (int)s.size(), out.data(), needed);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string out(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), needed, nullptr, nullptr);
    return out;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();

    // +1 for the null terminator; GlobalAlloc size is in bytes.
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) {
        CloseClipboard();
        return false;
    }

    void* dst = GlobalLock(hMem);
    if (!dst) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }
    memcpy(dst, text.c_str(), bytes);
    GlobalUnlock(hMem);

    // Ownership of hMem transfers to the clipboard on success; do not free it ourselves.
    bool ok = SetClipboardData(CF_UNICODETEXT, hMem) != nullptr;
    if (!ok) GlobalFree(hMem);

    CloseClipboard();
    return ok;
}

bool IsElevated() {
    bool elevated = false;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(hToken);
    }
    return elevated;
}

std::wstring RunCommandCaptureOutput(const std::wstring& commandLine, DWORD timeoutMs) {
    std::wstring result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return L"ERROR: failed to create pipe";
    }
    // Ensure the read handle is not inherited by the child.
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    std::wstring cmdLine = L"cmd.exe /c " + commandLine;
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWritePipe); // parent doesn't need the write end

    if (!ok) {
        CloseHandle(hReadPipe);
        return L"ERROR: failed to launch command";
    }

    // Read available output while the process runs (avoids pipe buffer deadlock),
    // then wait (bounded) for exit.
    std::string rawOutput;
    char buffer[4096];
    DWORD bytesRead = 0;
    DWORD elapsed = 0;
    for (;;) {
        DWORD waitResult = WaitForSingleObject(pi.hProcess, 50);
        BOOL readOk = ReadFile(hReadPipe, buffer, sizeof(buffer), &bytesRead, nullptr);
        if (readOk && bytesRead > 0) {
            rawOutput.append(buffer, bytesRead);
        }
        if (waitResult == WAIT_OBJECT_0 && (!readOk || bytesRead == 0)) break;
        elapsed += 50;
        if (elapsed >= timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    // Drain any remaining buffered output.
    while (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        rawOutput.append(buffer, bytesRead);
    }

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    result = AnsiToWide(rawOutput, CP_OEMCP);
    return result;
}
