#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <shlwapi.h>
#include <wrl.h>
#include <WebView2.h>

#include <fstream>
#include <locale>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CpeProtocol.h"

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Crypt32.lib")

using Microsoft::WRL::ComPtr;

static HWND g_window = nullptr;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static std::wstring g_folder;
static CpeProtocol g_cpeProtocol;
static std::mutex g_controlRequestMutex;

static std::wstring PathOf(const wchar_t* name) { return g_folder + L"\\" + name; }

static std::wstring JsonEscape(const std::wstring& value) {
    std::wstring result;
    for (const wchar_t character : value) {
        if (character == L'\\' || character == L'\"') { result += L'\\'; result += character; }
        else if (character == L'\n') result += L"\\n";
        else if (character == L'\r') result += L"\\r";
        else if (character == L'\t') result += L"\\t";
        else if (character >= 0x20) result += character;
    }
    return result;
}

static std::wstring ReadText(const std::wstring& path) {
    std::wifstream input(path);
    input.imbue(std::locale(""));
    return std::wstring((std::istreambuf_iterator<wchar_t>(input)), std::istreambuf_iterator<wchar_t>());
}

// Credentials are protected with Windows DPAPI.  They can be decrypted only
// by the same Windows user account that saved them.
static std::wstring ReadSavedLogin() {
    std::ifstream input(PathOf(L"cpe_login.dat"), std::ios::binary);
    if (!input) return L"";
    const std::vector<BYTE> encrypted((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (encrypted.empty()) return L"";

    DATA_BLOB source{ static_cast<DWORD>(encrypted.size()), const_cast<BYTE*>(encrypted.data()) };
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) return L"";
    const std::wstring result(reinterpret_cast<const wchar_t*>(plain.pbData), plain.cbData / sizeof(wchar_t));
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return result;
}

static std::wstring GetLineValue(const std::wstring& text, const std::wstring& key) {
    const size_t begin = text.find(key + L"=");
    if (begin == std::wstring::npos) return L"";
    const size_t valueBegin = begin + key.size() + 1;
    const size_t end = text.find_first_of(L"\r\n", valueBegin);
    return text.substr(valueBegin, end == std::wstring::npos ? std::wstring::npos : end - valueBegin);
}

static bool ReconnectSavedCpe(std::wstring& error) {
    const std::wstring saved = ReadSavedLogin();
    const std::wstring url = GetLineValue(saved, L"url");
    const std::wstring password = GetLineValue(saved, L"password");
    if (url.empty() || password.empty()) {
        error = L"缺少 CPE 登录信息，请重新连接 CPE 后再操作锁频。";
        return false;
    }
    return g_cpeProtocol.Login(url, password, error);
}

static bool RefreshLockSession(std::wstring& error) {
    // Huawei invalidates lock-write tokens after ordinary polling requests.
    // A fresh SCRAM session gives the immediately following write its own token.
    if (!ReconnectSavedCpe(error)) return false;
    if (g_cpeProtocol.UsedChallengeLogin()) return true;
    error = L"CPE 未建立锁频所需的高权限会话，请稍候重新连接后再试。";
    return false;
}

static void SaveLogin(const std::wstring& url, const std::wstring& password) {
    std::wstring saved = L"url=" + url + L"\npassword=" + password;
    DATA_BLOB source{ static_cast<DWORD>(saved.size() * sizeof(wchar_t)), reinterpret_cast<BYTE*>(saved.data()) };
    DATA_BLOB encrypted{};
    if (CryptProtectData(&source, L"CPEManager credentials", nullptr, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) {
        std::ofstream output(PathOf(L"cpe_login.dat"), std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encrypted.pbData), encrypted.cbData);
        output.close();
        SecureZeroMemory(encrypted.pbData, encrypted.cbData);
        LocalFree(encrypted.pbData);
        // Remove the legacy plaintext file after a successful encrypted save.
        DeleteFileW(PathOf(L"cpe_login.txt").c_str());
    }
    SecureZeroMemory(saved.data(), saved.size() * sizeof(wchar_t));
}

static std::wstring JsonValue(const std::wstring& json, const std::wstring& key) {
    const std::wstring marker = L"\"" + key + L"\"";
    size_t start = json.find(marker);
    if (start == std::wstring::npos) return L"";
    start = json.find(L':', start);
    if (start == std::wstring::npos) return L"";
    ++start;
    while (start < json.size() && iswspace(json[start])) ++start;
    const bool quoted = start < json.size() && json[start] == L'\"';
    if (quoted) ++start;
    std::wstring value;
    for (size_t index = start; index < json.size(); ++index) {
        if (json[index] == L'\\' && index + 1 < json.size()) {
            const wchar_t escaped = json[++index];
            value += escaped == L'n' ? L'\n' : escaped == L'r' ? L'\r' : escaped == L't' ? L'\t' : escaped;
            continue;
        }
        if ((quoted && json[index] == L'\"') || (!quoted && (json[index] == L',' || json[index] == L'}' || iswspace(json[index])))) break;
        value += json[index];
    }
    return value;
}

static std::wstring SignalJson(bool refresh) {
    CpeSignalData signal;
    std::wstring error;
    if (!g_cpeProtocol.FetchSignals(signal, error)) {
        return L"{\"action\":\"cpe-data\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\",\"refresh\":true}";
    }
    return std::wstring(L"{\"action\":\"cpe-data\",\"status\":\"ok\",\"refresh\":") + (refresh ? L"true" : L"false") +
        L",\"rsrp\":\"" + JsonEscape(signal.rsrp) + L"\",\"rsrq\":\"" + JsonEscape(signal.rsrq) +
        L"\",\"rssi\":\"" + JsonEscape(signal.rssi) + L"\",\"sinr\":\"" + JsonEscape(signal.sinr) +
        L"\",\"downloadRate\":\"" + JsonEscape(signal.downloadRate) + L"\",\"uploadRate\":\"" + JsonEscape(signal.uploadRate) +
        L"\",\"operatorName\":\"" + JsonEscape(signal.operatorName) + L"\",\"networkType\":\"" + JsonEscape(signal.networkType) +
        L"\",\"currentUpload\":\"" + JsonEscape(signal.currentUpload) + L"\",\"currentDownload\":\"" + JsonEscape(signal.currentDownload) +
        L"\",\"totalUpload\":\"" + JsonEscape(signal.totalUpload) + L"\",\"totalDownload\":\"" + JsonEscape(signal.totalDownload) +
        L"\",\"currentConnectTime\":\"" + JsonEscape(signal.currentConnectTime) + L"\",\"totalConnectTime\":\"" + JsonEscape(signal.totalConnectTime) +
        L"\",\"pccBand\":\"" + JsonEscape(signal.pccBand) + L"\",\"pccBandwidth\":\"" + JsonEscape(signal.pccBandwidth) +
        L"\",\"pccArfcn\":\"" + JsonEscape(signal.pccArfcn) + L"\",\"pccPci\":\"" + JsonEscape(signal.pccPci) +
        L"\",\"sccPresent\":" + (signal.hasScc ? L"true" : L"false") + L",\"sccBand\":\"" + JsonEscape(signal.sccBand) +
        L"\",\"sccBandwidth\":\"" + JsonEscape(signal.sccBandwidth) + L"\",\"sccArfcn\":\"" + JsonEscape(signal.sccArfcn) +
        L"\",\"sccPci\":\"" + JsonEscape(signal.sccPci) + L"\",\"sccRsrp\":\"" + JsonEscape(signal.sccRsrp) +
        L"\",\"sccRsrq\":\"" + JsonEscape(signal.sccRsrq) + L"\",\"sccRssi\":\"" + JsonEscape(signal.sccRssi) +
        L"\",\"sccSinr\":\"" + JsonEscape(signal.sccSinr) + L"\",\"cellId\":\"" + JsonEscape(signal.cellId) +
        L"\",\"mcsUp\":\"" + JsonEscape(signal.mcsUp) + L"\",\"mcsDown\":\"" + JsonEscape(signal.mcsDown) +
        L"\",\"rank\":\"" + JsonEscape(signal.rank) + L"\",\"cqi\":\"" + JsonEscape(signal.cqi) +
        L"\",\"plmn\":\"" + JsonEscape(signal.plmn) + L"\",\"bandSummary\":\"" + JsonEscape(signal.bandSummary) +
        L"\",\"accessCode\":\"" + JsonEscape(signal.accessCode) + L"\"}";
}

static std::wstring ContractRateJson() {
    using GetRates = BOOL (WINAPI*)(const wchar_t*, const wchar_t*, wchar_t*, DWORD, wchar_t*, DWORD, wchar_t*, DWORD);
    const std::wstring saved = ReadSavedLogin();
    const std::wstring url = GetLineValue(saved, L"url");
    const std::wstring password = GetLineValue(saved, L"password");
    HMODULE plugin = LoadLibraryW(PathOf(L"CpeContractRate.dll").c_str());
    if (!plugin) return L"{\"action\":\"cpe-contract-rate\",\"status\":\"error\",\"error\":\"未找到 CpeContractRate.dll。\"}";
    const GetRates getRates = reinterpret_cast<GetRates>(GetProcAddress(plugin, "GetCpeContractRatesW"));
    if (!getRates) {
        FreeLibrary(plugin);
        return L"{\"action\":\"cpe-contract-rate\",\"status\":\"error\",\"error\":\"签约速率 DLL 接口无效。\"}";
    }
    wchar_t downlink[64]{}, uplink[64]{}, error[512]{};
    const BOOL ok = getRates(url.c_str(), password.c_str(), downlink, ARRAYSIZE(downlink), uplink, ARRAYSIZE(uplink), error, ARRAYSIZE(error));
    FreeLibrary(plugin);
    if (!ok) return L"{\"action\":\"cpe-contract-rate\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\"}";
    return L"{\"action\":\"cpe-contract-rate\",\"status\":\"ok\",\"downlinkRate\":\"" + JsonEscape(downlink) +
        L"\",\"uplinkRate\":\"" + JsonEscape(uplink) + L"\"}";
}

static std::vector<std::wstring> CsvValues(const std::wstring& text) {
    std::vector<std::wstring> values;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find(L',', begin);
        const std::wstring value = text.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        if (!value.empty()) values.push_back(value);
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return values;
}

static bool DigitsOnly(const std::wstring& value) {
    if (value.empty()) return false;
    for (const wchar_t character : value) if (character < L'0' || character > L'9') return false;
    return true;
}

static bool UnsignedInRange(const std::wstring& value, unsigned long maximum) {
    if (!DigitsOnly(value)) return false;
    try { return std::stoul(value) <= maximum; } catch (...) { return false; }
}

static bool SupportedBand(bool nr, const std::wstring& band) {
    static const wchar_t* lte[] = { L"1", L"3", L"5", L"7", L"8", L"20", L"28", L"38", L"39", L"40", L"41", L"42", L"43" };
    static const wchar_t* fiveG[] = { L"1", L"3", L"5", L"7", L"8", L"20", L"28", L"38", L"40", L"41", L"71", L"77", L"78" };
    const wchar_t** values = nr ? fiveG : lte;
    const size_t count = nr ? ARRAYSIZE(fiveG) : ARRAYSIZE(lte);
    for (size_t index = 0; index < count; ++index) if (band == values[index]) return true;
    return false;
}

static std::wstring NormalizePhone(const std::wstring& value) {
    std::wstring phone;
    for (const wchar_t character : value) {
        if (character == L' ' || character == L'-' || character == L'(' || character == L')') continue;
        phone += character;
    }
    return phone;
}

static bool ValidPhone(const std::wstring& phone) {
    if (phone.size() < 3 || phone.size() > 32) return false;
    for (size_t index = 0; index < phone.size(); ++index) {
        if (phone[index] >= L'0' && phone[index] <= L'9') continue;
        if (index == 0 && phone[index] == L'+') continue;
        return false;
    }
    return true;
}

static std::wstring SmsListJson(bool sent) {
    std::vector<CpeSmsMessage> messages;
    size_t totalCount = 0;
    std::wstring error;
    if ((!g_cpeProtocol.IsConnected() && !ReconnectSavedCpe(error)) ||
        !g_cpeProtocol.FetchSmsMessages(sent, messages, totalCount, error)) {
        return L"{\"action\":\"cpe-sms-list\",\"status\":\"error\",\"box\":\"" +
            std::wstring(sent ? L"outbox" : L"inbox") + L"\",\"error\":\"" + JsonEscape(error) + L"\"}";
    }
    CpeSmsCounts counts;
    std::wstring countError;
    if (g_cpeProtocol.FetchSmsCounts(counts, countError)) {
        totalCount = sent ? counts.localOutbox + counts.simOutbox : counts.localInbox + counts.simInbox;
    }
    std::wstring json = L"{\"action\":\"cpe-sms-list\",\"status\":\"ok\",\"box\":\"" +
        std::wstring(sent ? L"outbox" : L"inbox") + L"\",\"totalCount\":" +
        std::to_wstring(totalCount) + L",\"messages\":[";
    for (size_t index = 0; index < messages.size(); ++index) {
        if (index) json += L',';
        const CpeSmsMessage& message = messages[index];
        json += L"{\"index\":\"" + JsonEscape(message.index) + L"\",\"phone\":\"" +
            JsonEscape(message.phone) + L"\",\"content\":\"" + JsonEscape(message.content) +
            L"\",\"date\":\"" + JsonEscape(message.date) + L"\",\"unread\":" +
            (message.unread ? L"true" : L"false") + L"}";
    }
    return json + L"]}";
}

static std::wstring SmsSendJson(const std::wstring& rawPhone, const std::wstring& content) {
    const std::wstring phone = NormalizePhone(rawPhone);
    if (!ValidPhone(phone)) {
        return L"{\"action\":\"cpe-sms-send\",\"status\":\"error\",\"error\":\"请输入有效的收件人号码。\"}";
    }
    if (content.empty()) {
        return L"{\"action\":\"cpe-sms-send\",\"status\":\"error\",\"error\":\"短信内容不能为空。\"}";
    }
    if (content.size() > 1000) {
        return L"{\"action\":\"cpe-sms-send\",\"status\":\"error\",\"error\":\"短信内容不能超过 1000 个字符。\"}";
    }
    std::wstring error;
    if ((!g_cpeProtocol.IsConnected() && !ReconnectSavedCpe(error)) ||
        !g_cpeProtocol.SendSms(phone, content, error)) {
        return L"{\"action\":\"cpe-sms-send\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\"}";
    }
    return L"{\"action\":\"cpe-sms-send\",\"status\":\"ok\",\"message\":\"短信已提交到 CPE 发送队列。\"}";
}

static std::wstring ControlStateJson() {
    CpeNetworkMode mode;
    std::wstring error;
    if ((!g_cpeProtocol.IsConnected() && !ReconnectSavedCpe(error)) || !g_cpeProtocol.FetchNetworkMode(mode, error)) {
        return L"{\"action\":\"cpe-control\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\"}";
    }
    return L"{\"action\":\"cpe-control\",\"status\":\"ok\",\"networkMode\":\"" + JsonEscape(mode.mode) +
        L"\",\"networkOption\":\"" + JsonEscape(mode.networkOption) + L"\"}";
}

static std::wstring TerminalDevicesJson() {
    std::vector<CpeTerminalDevice> devices;
    std::vector<CpeTerminalDevice> availableDevices;
    std::vector<CpeTerminalDevice> historyDevices;
    std::wstring error;
    if ((!g_cpeProtocol.IsConnected() && !ReconnectSavedCpe(error)) || !g_cpeProtocol.FetchConnectedDevices(devices, error)) {
        return L"{\"action\":\"cpe-acceleration-devices\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\"}";
    }
    std::wstring availableError;
    if (!g_cpeProtocol.FetchAvailableDevices(availableDevices, availableError)) availableDevices = devices;
    std::wstring historyError;
    const bool historyAvailable = g_cpeProtocol.FetchAccelerationHistory(historyDevices, historyError);
    auto normalizeMac = [](std::wstring value) {
        std::wstring normalized;
        for (const wchar_t character : value) if (iswxdigit(character)) normalized += static_cast<wchar_t>(towupper(character));
        return normalized;
    };
    for (CpeTerminalDevice& history : historyDevices) {
        const auto terminal = std::find_if(availableDevices.begin(), availableDevices.end(), [&](const CpeTerminalDevice& current) {
            return normalizeMac(current.mac) == normalizeMac(history.mac);
        });
        if (terminal != availableDevices.end() && !terminal->name.empty()) history.name = terminal->name;
    }
    const auto appendDevices = [](std::wstring& json, const std::vector<CpeTerminalDevice>& source) {
        for (size_t index = 0; index < source.size(); ++index) {
            if (index) json += L",";
            const CpeTerminalDevice& device = source[index];
            json += L"{\"id\":\"" + JsonEscape(device.mac) + L"\",\"name\":\"" + JsonEscape(device.name) +
                L"\",\"detail\":\"" + JsonEscape((device.status == L"1" || device.status == L"online") ? L"在线 · " : L"已连接 · ") + JsonEscape(device.mac) +
                L"\",\"duration\":\"" + JsonEscape(device.status) + L"\",\"totalDuration\":\"" + JsonEscape(device.totalDuration) + L"\"}";
        }
    };
    std::wstring json = L"{\"action\":\"cpe-acceleration-devices\",\"status\":\"ok\",\"devices\":[";
    appendDevices(json, devices);
    json += L"],\"availableDevices\":[";
    appendDevices(json, availableDevices);
    json += L"],\"historyStatus\":\"" + std::wstring(historyAvailable ? L"ok" : L"error") + L"\",\"history\":[";
    if (historyAvailable) appendDevices(json, historyDevices);
    return json + L"]}";
}

static std::wstring DeviceInformationJson() {
    CpeDeviceInformation information;
    std::wstring error;
    if ((!g_cpeProtocol.IsConnected() && !ReconnectSavedCpe(error)) || !g_cpeProtocol.FetchDeviceInformation(information, error)) {
        return L"{\"action\":\"cpe-device-information\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\"}";
    }
    return L"{\"action\":\"cpe-device-information\",\"status\":\"ok\",\"model\":\"" + JsonEscape(information.model) +
        L"\",\"uptime\":\"" + JsonEscape(information.uptime) +
        L"\",\"serialNumber\":\"" + JsonEscape(information.serialNumber) +
        L"\",\"imei\":\"" + JsonEscape(information.imei) +
        L"\",\"imsi\":\"" + JsonEscape(information.imsi) +
        L"\",\"phoneNumber\":\"" + JsonEscape(information.phoneNumber) +
        L"\",\"hardwareVersion\":\"" + JsonEscape(information.hardwareVersion) +
        L"\",\"softwareVersion\":\"" + JsonEscape(information.softwareVersion) +
        L"\",\"webUiVersion\":\"" + JsonEscape(information.webUiVersion) +
        L"\",\"configVersion\":\"" + JsonEscape(information.configVersion) +
        L"\",\"parameterVersion\":\"" + JsonEscape(information.parameterVersion) + L"\"}";
}

static std::wstring SetMobileDataJson(bool enabled) {
    std::wstring error;
    if ((!g_cpeProtocol.IsConnected() && !ReconnectSavedCpe(error)) || !g_cpeProtocol.SetMobileData(enabled, error)) {
        return L"{\"action\":\"cpe-control\",\"status\":\"error\",\"control\":\"mobile-data\",\"error\":\"" + JsonEscape(error) + L"\"}";
    }
    return L"{\"action\":\"cpe-control\",\"status\":\"ok\",\"control\":\"mobile-data\",\"enabled\":" +
        std::wstring(enabled ? L"true" : L"false") + L",\"message\":\"移动数据已" + (enabled ? L"开启。" : L"关闭。") + L"\"}";
}

static std::wstring SetNetworkControlJson(const std::wstring& preference, const std::wstring& fiveGMode) {
    CpeNetworkMode mode;
    std::wstring error;
    if ((!g_cpeProtocol.IsConnected() && !ReconnectSavedCpe(error)) || !g_cpeProtocol.FetchNetworkMode(mode, error)) {
        return L"{\"action\":\"cpe-control\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\"}";
    }
    if (!preference.empty()) {
        if (preference == L"auto" || preference == L"5g") mode.mode = L"00";
        else if (preference == L"4g") mode.mode = L"03";
        else return L"{\"action\":\"cpe-control\",\"status\":\"error\",\"error\":\"未知的首选网络方式。\"}";
    }
    if (!fiveGMode.empty()) {
        if (fiveGMode == L"nsa") mode.networkOption = L"0";
        else if (fiveGMode == L"sa") mode.networkOption = L"1";
        else if (fiveGMode == L"sa-nsa") mode.networkOption = L"2";
        else return L"{\"action\":\"cpe-control\",\"status\":\"error\",\"error\":\"未知的 5G 组网方式。\"}";
    }
    if (!g_cpeProtocol.SetNetworkMode(mode, error)) {
        return L"{\"action\":\"cpe-control\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\"}";
    }
    return L"{\"action\":\"cpe-control\",\"status\":\"ok\",\"control\":\"network\",\"networkMode\":\"" +
        JsonEscape(mode.mode) + L"\",\"networkOption\":\"" + JsonEscape(mode.networkOption) + L"\",\"message\":\"网络方式已保存，CPE 将重新选网。\"}";
}

static std::wstring LockErrorJson(const std::wstring& error) {
    return L"{\"action\":\"cpe-lock\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\"}";
}

static std::wstring LockClearedJson() {
    return L"{\"action\":\"cpe-lock\",\"status\":\"ok\",\"locked\":false,"
        L"\"lteMode\":\"0\",\"lteBand\":\"\",\"lteArfcn\":\"\",\"ltePci\":\"\",\"lteBands\":\"\","
        L"\"nrMode\":\"0\",\"nrBand\":\"\",\"nrArfcn\":\"\",\"nrPci\":\"\",\"nrBands\":\"\","
        L"\"message\":\"已切换为自动模式并解除频段锁定。\"}";
}

static std::wstring LockStateJson(const std::wstring& message = L"") {
    CpeLockState state;
    std::wstring error;
    if (!g_cpeProtocol.FetchLockState(state, error)) return LockErrorJson(error);
    const bool locked = state.lte.mode != L"0" || state.nr.mode != L"0";
    return std::wstring(L"{\"action\":\"cpe-lock\",\"status\":\"ok\",\"locked\":") + (locked ? L"true" : L"false") +
        L",\"lteMode\":\"" + JsonEscape(state.lte.mode) + L"\",\"lteBand\":\"" + JsonEscape(state.lte.band) +
        L"\",\"lteArfcn\":\"" + JsonEscape(state.lte.arfcn) + L"\",\"ltePci\":\"" + JsonEscape(state.lte.pci) +
        L"\",\"lteBands\":\"" + JsonEscape(state.lte.allBands) + L"\",\"nrMode\":\"" + JsonEscape(state.nr.mode) +
        L"\",\"nrBand\":\"" + JsonEscape(state.nr.band) + L"\",\"nrArfcn\":\"" + JsonEscape(state.nr.arfcn) +
        L"\",\"nrPci\":\"" + JsonEscape(state.nr.pci) + L"\",\"nrBands\":\"" + JsonEscape(state.nr.allBands) +
        L"\",\"message\":\"" + JsonEscape(message) + L"\"}";
}

static std::wstring NeighborJson() {
    CpeNeighborData neighbors;
    std::wstring error;
    if (!g_cpeProtocol.FetchNeighborCells(neighbors, error)) {
        return L"{\"action\":\"cpe-neighbors\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\"}";
    }
    return L"{\"action\":\"cpe-neighbors\",\"status\":\"ok\",\"lteList\":\"" + JsonEscape(neighbors.lteList) +
        L"\",\"nrList\":\"" + JsonEscape(neighbors.nrList) + L"\"}";
}

static void PostJson(const std::wstring& json) {
    if (g_webview) g_webview->PostWebMessageAsJson(json.c_str());
}

static void PostControlStateAsync() {
    std::thread([] {
        std::lock_guard<std::mutex> lock(g_controlRequestMutex);
        std::wstring* json = new std::wstring(ControlStateJson());
        if (!PostMessageW(g_window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(json))) delete json;
    }).detach();
}

static void PostTerminalDevicesAsync() {
    std::thread([] {
        std::lock_guard<std::mutex> lock(g_controlRequestMutex);
        std::wstring* json = new std::wstring(TerminalDevicesJson());
        if (!PostMessageW(g_window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(json))) delete json;
    }).detach();
}

static void PostDeviceInformationAsync() {
    std::thread([] {
        std::lock_guard<std::mutex> lock(g_controlRequestMutex);
        std::wstring* json = new std::wstring(DeviceInformationJson());
        if (!PostMessageW(g_window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(json))) delete json;
    }).detach();
}

static void SetMobileDataAsync(bool enabled) {
    std::thread([enabled] {
        std::lock_guard<std::mutex> lock(g_controlRequestMutex);
        std::wstring* json = new std::wstring(SetMobileDataJson(enabled));
        if (!PostMessageW(g_window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(json))) delete json;
    }).detach();
}

static void SetNetworkControlAsync(std::wstring preference, std::wstring fiveGMode) {
    std::thread([preference = std::move(preference), fiveGMode = std::move(fiveGMode)] {
        std::lock_guard<std::mutex> lock(g_controlRequestMutex);
        std::wstring* json = new std::wstring(SetNetworkControlJson(preference, fiveGMode));
        if (!PostMessageW(g_window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(json))) delete json;
    }).detach();
}

static void ResizeWebView() {
    if (!g_controller) return;
    RECT bounds{};
    GetClientRect(g_window, &bounds);
    g_controller->put_Bounds(bounds);
}

static void HandleMessage(const std::wstring& message) {
    const std::wstring action = JsonValue(message, L"action");
    if (action == L"load-cpe-login") {
        const std::wstring saved = ReadSavedLogin();
        const std::wstring url = GetLineValue(saved, L"url").empty() ? L"http://192.168.1.1/" : GetLineValue(saved, L"url");
        PostJson(L"{\"action\":\"cpe-login-settings\",\"url\":\"" + JsonEscape(url) + L"\",\"password\":\"\"}");
    } else if (action == L"connect-cpe") {
        // A second queued click must reuse the established session instead of
        // requesting a new one-time Huawei verification token.
        if (g_cpeProtocol.IsConnected()) {
            PostJson(SignalJson(false));
            return;
        }
        const std::wstring url = JsonValue(message, L"url");
        std::wstring password = JsonValue(message, L"password");
        if (password.empty()) {
            const std::wstring saved = ReadSavedLogin();
            if (GetLineValue(saved, L"url") != url) {
                PostJson(L"{\"action\":\"cpe-data\",\"status\":\"error\",\"error\":\"Please enter the password for this CPE address.\"}");
                return;
            }
            password = GetLineValue(saved, L"password");
        }
        std::wstring error;
        if (g_cpeProtocol.Login(url, password, error)) {
            SaveLogin(url, password);
            PostJson(SignalJson(false));
        }
        else PostJson(L"{\"action\":\"cpe-data\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\"}");
    } else if (action == L"refresh-cpe") {
        std::wstring json = SignalJson(true);
        if (json.find(L"\"status\":\"error\"") != std::wstring::npos) {
            std::wstring error;
            if (ReconnectSavedCpe(error)) json = SignalJson(true);
            else json = L"{\"action\":\"cpe-data\",\"status\":\"error\",\"error\":\"" + JsonEscape(error) + L"\",\"refresh\":true}";
        }
        PostJson(json);
    } else if (action == L"refresh-contract-rate") {
        std::thread([] {
            std::wstring* json = new std::wstring(ContractRateJson());
            if (!PostMessageW(g_window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(json))) delete json;
        }).detach();
    } else if (action == L"refresh-control") {
        PostControlStateAsync();
    } else if (action == L"refresh-acceleration-devices") {
        PostTerminalDevicesAsync();
    } else if (action == L"refresh-device-information") {
        PostDeviceInformationAsync();
    } else if (action == L"set-mobile-data") {
        SetMobileDataAsync(JsonValue(message, L"enabled") == L"true");
    } else if (action == L"set-network-control") {
        SetNetworkControlAsync(JsonValue(message, L"preference"), JsonValue(message, L"fiveGMode"));
    } else if (action == L"refresh-lock-state") {
        PostJson(LockStateJson());
    } else if (action == L"refresh-neighbors") {
        PostJson(NeighborJson());
    } else if (action == L"refresh-sms") {
        PostJson(SmsListJson(JsonValue(message, L"box") == L"outbox"));
    } else if (action == L"send-sms") {
        PostJson(SmsSendJson(JsonValue(message, L"phone"), JsonValue(message, L"content")));
    } else if (action == L"clear-lock") {
        std::wstring error;
        PostJson(RefreshLockSession(error) && g_cpeProtocol.ClearFrequencyLock(error) ? LockClearedJson() : LockErrorJson(error));
    } else if (action == L"band-lock") {
        std::wstring error;
        const std::vector<std::wstring> lte = CsvValues(JsonValue(message, L"lteBands"));
        const std::vector<std::wstring> nr = CsvValues(JsonValue(message, L"nrBands"));
        bool valid = !lte.empty() || !nr.empty();
        for (const std::wstring& band : lte) valid = valid && SupportedBand(false, band);
        for (const std::wstring& band : nr) valid = valid && SupportedBand(true, band);
        if (!valid) PostJson(LockErrorJson(L"请选择有效的 4G 或 5G 频段。"));
        else PostJson(RefreshLockSession(error) && g_cpeProtocol.SetBandLock(lte, nr, error) ? LockStateJson(L"频段锁定已提交。") : LockErrorJson(error));
    } else if (action == L"cell-lock") {
        std::wstring band = JsonValue(message, L"band");
        const std::wstring arfcn = JsonValue(message, L"arfcn");
        const std::wstring pci = JsonValue(message, L"pci");
        const bool nr = band.size() >= 2 && band[0] == L'N';
        const bool lte = band.size() >= 2 && band[0] == L'B';
        if ((!nr && !lte) || !SupportedBand(nr, band.substr(1)) ||
            !UnsignedInRange(arfcn, nr ? 3279165UL : 262143UL) || !UnsignedInRange(pci, nr ? 1007UL : 503UL)) {
            PostJson(LockErrorJson(L"请选择 4G/5G，并输入有效的频段、频点和 PCI。"));
        } else {
            band.erase(0, 1);
            std::wstring error;
            PostJson(RefreshLockSession(error) && g_cpeProtocol.SetCellLock(nr, band, arfcn, pci, error) ? LockStateJson(L"小区锁定已提交。") : LockErrorJson(error));
        }
    }
}

static void NavigateUi() {
    if (!g_webview) return;
    const std::wstring file = PathOf(L"index.html");
    wchar_t url[32768]{};
    DWORD urlLength = ARRAYSIZE(url);
    if (SUCCEEDED(UrlCreateFromPathW(file.c_str(), url, &urlLength, 0))) g_webview->Navigate(url);
}

static void InitializeWebView() {
    const std::wstring dataFolder = PathOf(L"WebViewData");
    CreateCoreWebView2EnvironmentWithOptions(nullptr, dataFolder.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result)) return result;
                return environment->CreateCoreWebView2Controller(g_window,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controllerResult)) return controllerResult;
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);
                            ComPtr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(FALSE);
                            ResizeWebView();
                            EventRegistrationToken token{};
                            g_webview->add_WebMessageReceived(Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                    LPWSTR raw = nullptr;
                                    if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                        HandleMessage(raw);
                                        CoTaskMemFree(raw);
                                    }
                                    return S_OK;
                                }).Get(), &token);
                            const std::wstring injectedUi = ReadText(PathOf(L"acceleration-ui.js")) + L"\n" + ReadText(PathOf(L"parameters-ui.js"));
                            if (!injectedUi.empty()) {
                                g_webview->AddScriptToExecuteOnDocumentCreated(injectedUi.c_str(),
                                    Microsoft::WRL::Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                                        [](HRESULT, LPCWSTR) -> HRESULT { NavigateUi(); return S_OK; }).Get());
                            } else NavigateUi();
                            return S_OK;
                        }).Get());
            }).Get());
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE: ResizeWebView(); return 0;
    case WM_APP + 1: {
        std::wstring* json = reinterpret_cast<std::wstring*>(lParam);
        if (json) { PostJson(*json); delete json; }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int command) {
    SetProcessDPIAware();
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    PathRemoveFileSpecW(module);
    g_folder = module;
#ifdef CPE_API_SELF_TEST
    const std::wstring saved = ReadSavedLogin();
    const std::wstring url = GetLineValue(saved, L"url");
    const std::wstring password = GetLineValue(saved, L"password");
    if (url.empty() || password.empty()) return 5;
    std::wstring error;
    if (g_cpeProtocol.Login(url, password, error)) {
        CpeSignalData signal;
        if (!g_cpeProtocol.FetchSignals(signal, error)) return 8;
        if (signal.downloadRate.empty() || signal.uploadRate.empty() || signal.currentDownload.empty() || signal.totalDownload.empty()) return 9;
        return g_cpeProtocol.UsedChallengeLogin() ? 0 : 7;
    }
    if (error.find(L"108007") != std::wstring::npos) return 3;
    if (error.find(L"125003") != std::wstring::npos) return 2;
    if (error.find(L"网络错误") != std::wstring::npos) return 6;
    return 4;
#endif
    const wchar_t className[] = L"CPEManagerNativeWindow";
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = className;
    windowClass.lpfnWndProc = WindowProc;
    RegisterClassW(&windowClass);
    RECT rectangle{ 0, 0, 570, 1200 };
    AdjustWindowRectEx(&rectangle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    g_window = CreateWindowExW(0, className, L"CPEManager_V1.0", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
        nullptr, nullptr, instance, nullptr);
    ShowWindow(g_window, command);
    InitializeWebView();
    MSG event{};
    while (GetMessageW(&event, nullptr, 0, 0)) { TranslateMessage(&event); DispatchMessageW(&event); }
    return 0;
}
