#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>

struct CpeSignalData {
    std::wstring rsrp;
    std::wstring rsrq;
    std::wstring rssi;
    std::wstring sinr;
    std::wstring downloadRate;
    std::wstring uploadRate;
    std::wstring operatorName;
    std::wstring networkType;
    std::wstring currentUpload;
    std::wstring currentDownload;
    std::wstring totalUpload;
    std::wstring totalDownload;
    std::wstring currentConnectTime;
    std::wstring totalConnectTime;
    std::wstring pccBand;
    std::wstring pccBandwidth;
    std::wstring pccArfcn;
    std::wstring pccPci;
    std::wstring sccBand;
    std::wstring sccBandwidth;
    std::wstring sccArfcn;
    std::wstring sccPci;
    std::wstring sccRsrp;
    std::wstring sccRsrq;
    std::wstring sccRssi;
    std::wstring sccSinr;
    std::wstring cellId;
    std::wstring mcsUp;
    std::wstring mcsDown;
    std::wstring rank;
    std::wstring cqi;
    std::wstring plmn;
    std::wstring bandSummary;
    std::wstring accessCode;
    bool hasScc = false;
};

struct CpeContractRateData {
    // Raw bit/s values reported by the SIM subscription profile.
    std::wstring downlinkRate;
    std::wstring uplinkRate;
};

struct CpeNetworkMode {
    std::wstring mode;
    std::wstring networkBand;
    std::wstring lteBand;
    std::wstring networkOption;
};

struct CpeTerminalDevice {
    std::wstring name;
    std::wstring mac;
    std::wstring status;
    std::wstring totalDuration;
};

struct CpeDeviceInformation {
    std::wstring model;
    std::wstring uptime;
    std::wstring serialNumber;
    std::wstring imei;
    std::wstring imsi;
    std::wstring phoneNumber;
    std::wstring hardwareVersion;
    std::wstring softwareVersion;
    std::wstring webUiVersion;
    std::wstring configVersion;
    std::wstring parameterVersion;
};

struct CpeLockTarget {
    std::wstring mode;
    std::wstring band;
    std::wstring arfcn;
    std::wstring pci;
    std::wstring allBands;
};

struct CpeLockState {
    CpeLockTarget lte;
    CpeLockTarget nr;
};

struct CpeNeighborData {
    std::wstring lteList;
    std::wstring nrList;
};

struct CpeSmsMessage {
    std::wstring index;
    std::wstring phone;
    std::wstring content;
    std::wstring date;
    bool unread = false;
};

struct CpeSmsCounts {
    size_t localInbox = 0;
    size_t localOutbox = 0;
    size_t simInbox = 0;
    size_t simOutbox = 0;
};

// Native Huawei CPE protocol implementation.  It replaces the inspected Qt
// plugins with WinHTTP + BCrypt and deliberately has no vendor-DLL dependency.
class CpeProtocol {
public:
    CpeProtocol() = default;
    ~CpeProtocol();

    CpeProtocol(const CpeProtocol&) = delete;
    CpeProtocol& operator=(const CpeProtocol&) = delete;

    bool Login(const std::wstring& address, const std::wstring& password, std::wstring& error);
    bool FetchSignals(CpeSignalData& signals, std::wstring& error);
    bool FetchNeighborCells(CpeNeighborData& neighbors, std::wstring& error);
    bool FetchContractRates(CpeContractRateData& rates, std::wstring& error);
    bool FetchNetworkMode(CpeNetworkMode& mode, std::wstring& error);
    bool FetchConnectedDevices(std::vector<CpeTerminalDevice>& devices, std::wstring& error);
    bool FetchAvailableDevices(std::vector<CpeTerminalDevice>& devices, std::wstring& error);
    bool FetchDeviceInformation(CpeDeviceInformation& information, std::wstring& error);
    bool FetchAccelerationHistory(std::vector<CpeTerminalDevice>& devices, std::wstring& error);
    static bool ParseAccelerationRecords(const std::wstring& response, std::vector<CpeTerminalDevice>& devices);
    static bool ParseAccelerationHistoryRecords(const std::wstring& response, std::vector<CpeTerminalDevice>& devices);
    bool SetNetworkMode(const CpeNetworkMode& mode, std::wstring& error);
    bool SetMobileData(bool enabled, std::wstring& error);
    bool FetchLockState(CpeLockState& state, std::wstring& error);
    bool SetBandLock(const std::vector<std::wstring>& lteBands,
                     const std::vector<std::wstring>& nrBands, std::wstring& error);
    bool SetCellLock(bool nr, const std::wstring& band, const std::wstring& arfcn,
                     const std::wstring& pci, std::wstring& error);
    bool ClearFrequencyLock(std::wstring& error);
    bool FetchSmsMessages(bool sent, std::vector<CpeSmsMessage>& messages,
                          size_t& totalCount, std::wstring& error);
    bool FetchSmsCounts(CpeSmsCounts& counts, std::wstring& error);
    bool SendSms(const std::wstring& phone, const std::wstring& content, std::wstring& error);
    bool IsConnected() const { return m_authenticated && !m_cookie.empty(); }
    bool UsedChallengeLogin() const { return m_usedChallengeLogin; }
    void Disconnect();

private:
    struct Response {
        DWORD status = 0;
        DWORD networkError = ERROR_SUCCESS;
        std::wstring body;
    };

    bool Open(const std::wstring& address, std::wstring& error);
    bool AcquireSession(std::wstring& error);
    bool LoginChallenge(const std::wstring& password, std::wstring& error);
    bool LoginType4(const std::wstring& password, std::wstring& error);
    std::wstring NextVerificationToken(bool challengeToken = false);
    Response Request(const wchar_t* method, const std::wstring& endpoint,
                     const std::wstring& body = L"", const std::wstring& token = L"",
                     const std::wstring& contentType = L"");
    void CaptureResponseState(HINTERNET request);
    std::wstring ResponseHeader(HINTERNET request, const wchar_t* name) const;
    bool ApplyFrequencyLock(const std::wstring& body, std::wstring& error);

    HINTERNET m_session = nullptr;
    HINTERNET m_connection = nullptr;
    bool m_https = false;
    bool m_authenticated = false;
    bool m_usedChallengeLogin = false;
    std::wstring m_host;
    std::wstring m_basePath;
    std::wstring m_cookie;
    std::wstring m_loginToken;
    std::vector<std::wstring> m_verificationTokens;
};
