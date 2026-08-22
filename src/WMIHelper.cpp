#include "WMIHelper.h"
#define _WIN32_DCOM
#include <windows.h>
#include <comdef.h>
#include <wbemidl.h>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

WMIHelper::WMIHelper() {}
WMIHelper::~WMIHelper() { Shutdown(); }

bool WMIHelper::Initialize() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE means COM was already initialized on this thread with a
    // different concurrency model (e.g. by the GUI framework) - treat as success,
    // we simply won't be the one to uninitialize it.
    if (SUCCEEDED(hr)) {
        m_comInitialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    // Security initialization can only succeed once per process; ignore failure
    // if it was already set elsewhere.
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                          RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                          nullptr, EOAC_NONE, nullptr);

    IWbemLocator* pLoc = nullptr;
    HRESULT hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                                     IID_IWbemLocator, (LPVOID*)&pLoc);
    if (FAILED(hres)) return false;

    IWbemServices* pSvc = nullptr;
    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr,
                                0, nullptr, nullptr, &pSvc);
    if (FAILED(hres)) {
        pLoc->Release();
        return false;
    }

    hres = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                              RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                              nullptr, EOAC_NONE);
    if (FAILED(hres)) {
        pSvc->Release();
        pLoc->Release();
        return false;
    }

    m_pLoc = pLoc;
    m_pSvc = pSvc;
    m_ready = true;
    return true;
}

void WMIHelper::Shutdown() {
    if (m_pSvc) { ((IWbemServices*)m_pSvc)->Release(); m_pSvc = nullptr; }
    if (m_pLoc) { ((IWbemLocator*)m_pLoc)->Release(); m_pLoc = nullptr; }
    if (m_comInitialized) { CoUninitialize(); m_comInitialized = false; }
    m_ready = false;
}

static std::wstring VariantToWString(const VARIANT& v) {
    if (v.vt == VT_BSTR) return v.bstrVal ? v.bstrVal : L"";
    if (v.vt == VT_I4) return std::to_wstring(v.lVal);
    if (v.vt == VT_UI4) return std::to_wstring(v.ulVal);
    if (v.vt == VT_I8) return std::to_wstring(v.llVal);
    if (v.vt == VT_UI8) return std::to_wstring(v.ullVal);
    if (v.vt == VT_BOOL) return v.boolVal ? L"True" : L"False";
    if (v.vt == VT_NULL || v.vt == VT_EMPTY) return L"";
    return L"";
}

std::vector<std::map<std::wstring, std::wstring>> WMIHelper::Query(const std::wstring& wql) {
    std::vector<std::map<std::wstring, std::wstring>> rows;
    if (!m_ready || !m_pSvc) return rows;

    IWbemServices* pSvc = (IWbemServices*)m_pSvc;
    IEnumWbemClassObject* pEnum = nullptr;
    HRESULT hres = pSvc->ExecQuery(bstr_t(L"WQL"), bstr_t(wql.c_str()),
                                    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                    nullptr, &pEnum);
    if (FAILED(hres) || !pEnum) return rows;

    IWbemClassObject* pObj = nullptr;
    ULONG returned = 0;
    while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &returned) == S_OK && returned > 0) {
        std::map<std::wstring, std::wstring> row;
        pObj->BeginEnumeration(WBEM_FLAG_NONSYSTEM_ONLY);
        BSTR name = nullptr;
        VARIANT val;
        while (pObj->Next(0, &name, &val, nullptr, nullptr) == WBEM_S_NO_ERROR) {
            if (name) {
                row[name] = VariantToWString(val);
                SysFreeString(name);
                name = nullptr;
            }
            VariantClear(&val);
        }
        pObj->EndEnumeration();
        pObj->Release();
        rows.push_back(std::move(row));
    }
    pEnum->Release();
    return rows;
}
