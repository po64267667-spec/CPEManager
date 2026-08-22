#include "CpeProtocol.h"

#include <cwchar>

namespace {
void CopyText(const std::wstring& source, wchar_t* destination, DWORD capacity) {
    if (!destination || capacity == 0) return;
    wcsncpy_s(destination, capacity, source.c_str(), _TRUNCATE);
}
}

extern "C" __declspec(dllexport) BOOL WINAPI GetCpeContractRatesW(
    const wchar_t* address,
    const wchar_t* password,
    wchar_t* downlinkRate,
    DWORD downlinkCapacity,
    wchar_t* uplinkRate,
    DWORD uplinkCapacity,
    wchar_t* errorText,
    DWORD errorCapacity) {
    if (!address || !*address || !password) {
        CopyText(L"缺少 CPE 地址或密码。", errorText, errorCapacity);
        return FALSE;
    }

    CpeProtocol protocol;
    std::wstring error;
    if (!protocol.Login(address, password, error)) {
        CopyText(error, errorText, errorCapacity);
        return FALSE;
    }

    CpeContractRateData rates;
    if (!protocol.FetchContractRates(rates, error)) {
        CopyText(error, errorText, errorCapacity);
        return FALSE;
    }

    CopyText(rates.downlinkRate, downlinkRate, downlinkCapacity);
    CopyText(rates.uplinkRate, uplinkRate, uplinkCapacity);
    CopyText(L"", errorText, errorCapacity);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
