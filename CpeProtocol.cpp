#include "CpeProtocol.h"

#include <bcrypt.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <regex>
#include <thread>
#include <utility>

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace {
std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::wstring XmlValue(const std::wstring& xml, const std::wstring& name) {
    std::wsmatch match;
    const std::wregex expression(L"<" + name + L">([^<]*)</" + name + L">", std::regex_constants::icase);
    return std::regex_search(xml, match, expression) ? match[1].str() : L"";
}

std::wstring FirstXmlValue(const std::wstring& xml, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* name : names) {
        const std::wstring value = XmlValue(xml, name);
        if (!value.empty()) return value;
    }
    return L"";
}

std::string Base64(const std::string& source) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((source.size() + 2) / 3) * 4);
    for (size_t offset = 0; offset < source.size(); offset += 3) {
        unsigned int value = static_cast<unsigned char>(source[offset]) << 16;
        if (offset + 1 < source.size()) value |= static_cast<unsigned char>(source[offset + 1]) << 8;
        if (offset + 2 < source.size()) value |= static_cast<unsigned char>(source[offset + 2]);
        result += alphabet[(value >> 18) & 0x3f];
        result += alphabet[(value >> 12) & 0x3f];
        result += offset + 1 < source.size() ? alphabet[(value >> 6) & 0x3f] : '=';
        result += offset + 2 < source.size() ? alphabet[value & 0x3f] : '=';
    }
    return result;
}

// Huawei password_type=4: Base64URL(lowercase hexadecimal SHA-256(data)).
std::string PasswordType4Hash(const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, written = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &written, 0);
    std::vector<BYTE> object(objectSize), digest(32);
    BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0);
    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()), 0);
    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    static constexpr char hex[] = "0123456789abcdef";
    std::string hexadecimal;
    hexadecimal.reserve(64);
    for (const BYTE byte : digest) {
        hexadecimal += hex[byte >> 4];
        hexadecimal += hex[byte & 0x0f];
    }
    std::string encoded = Base64(hexadecimal);
    for (char& character : encoded) {
        if (character == '+') character = '-';
        else if (character == '/') character = '_';
    }
    return encoded;
}

std::wstring EscapeXml(const std::wstring& text) {
    std::wstring result;
    for (const wchar_t character : text) {
        switch (character) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        case L'\"': result += L"&quot;"; break;
        case L'\'': result += L"&apos;"; break;
        default: result += character; break;
        }
    }
    return result;
}

std::wstring UnescapeXml(std::wstring text) {
    const std::pair<const wchar_t*, const wchar_t*> entities[] = {
        { L"&lt;", L"<" }, { L"&gt;", L">" }, { L"&quot;", L"\"" },
        { L"&apos;", L"'" }, { L"&amp;", L"&" }
    };
    for (const auto& entity : entities) {
        size_t position = 0;
        while ((position = text.find(entity.first, position)) != std::wstring::npos) {
            text.replace(position, wcslen(entity.first), entity.second);
            position += wcslen(entity.second);
        }
    }
    return text;
}

std::wstring Hex(const std::vector<BYTE>& bytes) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(bytes.size() * 2);
    for (const BYTE value : bytes) { result += digits[value >> 4]; result += digits[value & 0x0f]; }
    return result;
}

std::vector<BYTE> HexBytes(const std::wstring& text) {
    auto nibble = [](wchar_t value) -> int {
        if (value >= L'0' && value <= L'9') return value - L'0';
        if (value >= L'a' && value <= L'f') return value - L'a' + 10;
        if (value >= L'A' && value <= L'F') return value - L'A' + 10;
        return -1;
    };
    if (text.empty() || text.size() % 2) return {};
    std::vector<BYTE> bytes;
    bytes.reserve(text.size() / 2);
    for (size_t index = 0; index < text.size(); index += 2) {
        const int high = nibble(text[index]), low = nibble(text[index + 1]);
        if (high < 0 || low < 0) return {};
        bytes.push_back(static_cast<BYTE>((high << 4) | low));
    }
    return bytes;
}

std::vector<BYTE> Sha256(const std::vector<BYTE>& input) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; DWORD objectSize = 0, written = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &written, 0);
    std::vector<BYTE> object(objectSize), digest(32);
    const NTSTATUS status = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) == 0 &&
        BCryptHashData(hash, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0 ? 0 : -1;
    if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0 ? digest : std::vector<BYTE>{};
}

std::vector<BYTE> HmacSha256(const std::vector<BYTE>& key, const std::vector<BYTE>& message) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; DWORD objectSize = 0, written = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return {};
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &written, 0);
    std::vector<BYTE> object(objectSize), digest(32);
    const NTSTATUS status = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) == 0 &&
        BCryptHashData(hash, const_cast<PUCHAR>(message.data()), static_cast<ULONG>(message.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0 ? 0 : -1;
    if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0 ? digest : std::vector<BYTE>{};
}

std::vector<BYTE> Bytes(const std::string& value) {
    return std::vector<BYTE>(value.begin(), value.end());
}

std::vector<BYTE> Pbkdf2Sha256(const std::wstring& password, const std::vector<BYTE>& salt, ULONGLONG iterations) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return {};
    const std::string utf8 = ToUtf8(password);
    std::vector<BYTE> output(32);
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(utf8.data())), static_cast<ULONG>(utf8.size()), const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()), iterations, output.data(), static_cast<ULONG>(output.size()), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0 ? output : std::vector<BYTE>{};
}

std::wstring RandomNonce() {
    std::vector<BYTE> bytes(32);
    return BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? Hex(bytes) : L"";
}

std::wstring Trim(const std::wstring& value) {
    const size_t begin = value.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) return L"";
    const size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::wstring> Split(const std::wstring& value, wchar_t separator) {
    std::vector<std::wstring> result;
    size_t begin = 0;
    while (begin <= value.size()) {
        const size_t end = value.find(separator, begin);
        result.push_back(Trim(value.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin)));
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return result;
}

void PopulateSecondaryCarrier(const std::wstring& nrList, CpeSignalData& signals) {
    signals.hasScc = false;
    signals.sccBand.clear();
    signals.sccBandwidth.clear();
    signals.sccArfcn.clear();
    signals.sccPci.clear();
    signals.sccRsrp.clear();
    signals.sccRsrq.clear();
    signals.sccRssi.clear();
    signals.sccSinr.clear();

    for (const std::wstring& row : Split(nrList, L';')) {
        if (row.empty()) continue;
        const std::vector<std::wstring> fields = Split(row, L',');
        if (fields.size() < 4 || fields[0].empty() || fields[1].empty() || fields[3].empty()) continue;
        if (fields[0] == signals.pccArfcn && fields[3] == signals.pccPci) continue;

        signals.sccArfcn = fields[0];
        signals.sccBand = fields[1];
        signals.sccBandwidth = fields[2];
        signals.sccPci = fields[3];
        if (fields.size() >= 8) {
            signals.sccRsrp = fields[4];
            signals.sccRsrq = fields[5];
            signals.sccRssi = fields[6];
            signals.sccSinr = fields[7];
        }
        signals.hasScc = true;
        return;
    }
}
}

CpeProtocol::~CpeProtocol() { Disconnect(); }

void CpeProtocol::Disconnect() {
    if (m_connection) WinHttpCloseHandle(m_connection);
    if (m_session) WinHttpCloseHandle(m_session);
    m_connection = nullptr;
    m_session = nullptr;
    m_authenticated = false;
    m_usedChallengeLogin = false;
    m_https = false;
    m_host.clear();
    m_basePath.clear();
    m_cookie.clear();
    m_loginToken.clear();
    m_verificationTokens.clear();
}

bool CpeProtocol::Open(const std::wstring& address, std::wstring& error) {
    Disconnect();
    std::wstring normalized = address;
    if (normalized.find(L"://") == std::wstring::npos) normalized = L"http://" + normalized;

    URL_COMPONENTS components{ sizeof(components) };
    wchar_t host[256]{};
    wchar_t path[1024]{};
    components.lpszHostName = host;
    components.dwHostNameLength = ARRAYSIZE(host);
    components.lpszUrlPath = path;
    components.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(normalized.c_str(), 0, 0, &components) || !host[0]) {
        error = L"登录地址无效。";
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"仅支持 HTTP 或 HTTPS 登录地址。";
        return false;
    }

    m_https = components.nScheme == INTERNET_SCHEME_HTTPS;
    m_host = host;
    m_basePath = path;
    if (m_basePath == L"/") m_basePath.clear();
    while (!m_basePath.empty() && m_basePath.back() == L'/') m_basePath.pop_back();
    m_session = WinHttpOpen(L"CPEManager/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!m_session) {
        error = L"无法创建网络会话（错误 " + std::to_wstring(GetLastError()) + L"）。";
        return false;
    }
    m_connection = WinHttpConnect(m_session, host, components.nPort, 0);
    if (!m_connection) {
        error = L"无法连接 CPE（错误 " + std::to_wstring(GetLastError()) + L"）。";
        Disconnect();
        return false;
    }
    return true;
}

std::wstring CpeProtocol::ResponseHeader(HINTERNET request, const wchar_t* name) const {
    DWORD bytes = 0;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return L"";
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return L"";
    value.resize(wcslen(value.c_str()));
    return value;
}

void CpeProtocol::CaptureResponseState(HINTERNET request) {
    const std::wstring setCookie = ResponseHeader(request, L"Set-Cookie");
    std::wsmatch cookie;
    if (std::regex_search(setCookie, cookie, std::wregex(L"SessionID=([^;\\s]+)", std::regex_constants::icase))) {
        m_cookie = L"SessionID=" + cookie[1].str();
    }

    const std::wstring tokenOne = ResponseHeader(request, L"__RequestVerificationTokenone");
    const std::wstring tokenTwo = ResponseHeader(request, L"__RequestVerificationTokentwo");
    if (!tokenOne.empty() || !tokenTwo.empty()) {
        m_verificationTokens.clear();
        if (!tokenOne.empty()) m_verificationTokens.push_back(tokenOne);
        if (!tokenTwo.empty()) m_verificationTokens.push_back(tokenTwo);
        return;
    }

    const std::wstring headerTokens = ResponseHeader(request, L"__RequestVerificationToken");
    if (headerTokens.empty()) return;
    std::vector<std::wstring> tokens;
    size_t start = 0;
    while (start <= headerTokens.size()) {
        const size_t end = headerTokens.find(L'#', start);
        const std::wstring token = headerTokens.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!token.empty()) tokens.push_back(token);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    m_verificationTokens = tokens;
}

CpeProtocol::Response CpeProtocol::Request(const wchar_t* method, const std::wstring& endpoint, const std::wstring& body, const std::wstring& token, const std::wstring& contentType) {
    Response response;
    if (!m_connection) { response.networkError = ERROR_INVALID_HANDLE; return response; }
    const std::wstring path = m_basePath + endpoint;
    HINTERNET request = WinHttpOpenRequest(m_connection, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, m_https ? WINHTTP_FLAG_SECURE : 0);
    if (!request) { response.networkError = GetLastError(); return response; }

    // Same wire format as the usable Huawei client: raw XML, SessionID cookie,
    // X-Requested-With, and the current one-time token for authenticated POSTs.
    std::wstring headers = L"X-Requested-With: XMLHttpRequest\r\n";
    if (!m_cookie.empty()) headers += L"Cookie: " + m_cookie + L"\r\n";
    if (!token.empty()) headers += L"__RequestVerificationToken: " + token + L"\r\n";
    if (!contentType.empty()) headers += L"Content-Type: " + contentType + L"\r\n";
    const std::string data = ToUtf8(body);
    if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L), data.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(data.data()), static_cast<DWORD>(data.size()), static_cast<DWORD>(data.size()), 0) || !WinHttpReceiveResponse(request, nullptr)) {
        response.networkError = GetLastError();
        WinHttpCloseHandle(request);
        return response;
    }
    DWORD statusSize = sizeof(response.status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    CaptureResponseState(request);

    std::string bytes;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        bytes.append(chunk, 0, read);
    }
    response.body = ToWide(bytes);
    WinHttpCloseHandle(request);
    return response;
}

bool CpeProtocol::AcquireSession(std::wstring& error) {
    // ponytail: this firmware accepts Type-4 only with the page CSRF token.
    const Response session = Request(L"GET", L"/");
    if (session.networkError != ERROR_SUCCESS) {
        error = L"无法读取 CPE 会话信息（网络错误 " + std::to_wstring(session.networkError) + L"）。";
        return false;
    }
    if (session.status != 200) { error = L"CPE 首页会话 HTTP " + std::to_wstring(session.status); return false; }
    const size_t marker = session.body.find(L"csrf_token");
    const size_t content = marker == std::wstring::npos ? marker : session.body.find(L"content=", marker);
    if (content == std::wstring::npos || content + 9 >= session.body.size() || (session.body[content + 8] != L'\'' && session.body[content + 8] != L'\"')) { error = L"CPE 首页未返回 CSRF 令牌。"; return false; }
    const wchar_t quote = session.body[content + 8];
    const size_t tokenBegin = content + 9;
    const size_t tokenEnd = session.body.find(quote, tokenBegin);
    if (tokenEnd == std::wstring::npos || tokenEnd == tokenBegin) { error = L"CPE 首页 CSRF 令牌无效。"; return false; }
    if (m_cookie.empty()) { error = L"CPE 首页未返回会话 Cookie。"; return false; }
    m_loginToken = session.body.substr(tokenBegin, tokenEnd - tokenBegin);
    return true;
}

std::wstring CpeProtocol::NextVerificationToken(bool challengeToken) {
    std::wstring token = m_loginToken;
    if (!m_verificationTokens.empty()) {
        // Huawei returns one-time tokens in consumption order, separated by '#'.
        token = m_verificationTokens.front();
        m_verificationTokens.erase(m_verificationTokens.begin());
    }
    // Challenge-capable Huawei firmware uses the last 32 characters of its
    // initial token, then returns a 32-character token for the final request.
    return challengeToken && token.size() > 32 ? token.substr(token.size() - 32) : token;
}

bool CpeProtocol::LoginType4(const std::wstring& password, std::wstring& error) {

    const std::string firstHash = PasswordType4Hash(ToUtf8(password));
    const std::wstring passwordValue = ToWide(PasswordType4Hash("admin" + firstHash + ToUtf8(m_loginToken)));
    if (passwordValue.empty()) {
        error = L"无法生成 CPE 登录摘要。";
        return false;
    }
    const std::wstring xml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><request><Username>admin</Username><Password>" + EscapeXml(passwordValue) + L"</Password><password_type>4</password_type></request>";
    const Response login = Request(L"POST", L"/api/user/login", xml, m_loginToken);
    if (login.networkError != ERROR_SUCCESS) {
        error = L"CPE 登录请求未送达（网络错误 " + std::to_wstring(login.networkError) + L"）。";
        return false;
    }
    if (XmlValue(login.body, L"response") != L"OK") {
        const std::wstring code = XmlValue(login.body, L"code");
        error = code == L"108007" ? L"CPE 已临时限制登录次数，请等待解除限制后重试。" : L"CPE 登录失败" + (code.empty() ? L"（HTTP " + std::to_wstring(login.status) + L"）。" : L"（错误码 " + code + L"）。");
        return false;
    }
    m_authenticated = true;
    m_usedChallengeLogin = false;
    return true;
}

bool CpeProtocol::LoginChallenge(const std::wstring& password, std::wstring& error) {
    const std::wstring firstNonce = RandomNonce();
    if (firstNonce.empty()) { error = L"无法生成登录随机数。"; return false; }
    const std::wstring challengeXml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><request><username>admin</username><firstnonce>" + firstNonce + L"</firstnonce><mode>1</mode></request>";
    const std::wstring initialChallengeToken = m_loginToken.size() > 32 ? m_loginToken.substr(m_loginToken.size() - 32) : m_loginToken;
    const Response challenge = Request(L"POST", L"/api/user/challenge_login", challengeXml, initialChallengeToken, L"application/x-www-form-urlencoded; charset=UTF-8");
    if (challenge.networkError != ERROR_SUCCESS) {
        error = L"CPE 挑战登录请求未送达（网络错误 " + std::to_wstring(challenge.networkError) + L"）。";
        return false;
    }
    const std::wstring salt = XmlValue(challenge.body, L"salt");
    const std::wstring serverNonce = XmlValue(challenge.body, L"servernonce");
    const std::wstring iterationText = XmlValue(challenge.body, L"iterations");
    const std::wstring challengeCode = XmlValue(challenge.body, L"code");
    ULONGLONG iterations = 0;
    try { iterations = std::stoull(iterationText); } catch (...) {}
    const std::vector<BYTE> saltBytes = HexBytes(salt);
    if (challenge.status != 200 || !challengeCode.empty() || serverNonce.empty() || saltBytes.empty() || iterations == 0 || iterations > 1000000) {
        error = L"CPE 挑战登录失败" + (challengeCode.empty() ? L"。" : L"（错误码 " + challengeCode + L"）。");
        return false;
    }
    const std::vector<BYTE> saltedPassword = Pbkdf2Sha256(password, saltBytes, iterations);
    const std::vector<BYTE> clientKey = HmacSha256(Bytes("Client Key"), saltedPassword);
    const std::vector<BYTE> storedKey = Sha256(clientKey);
    std::wstring authText = firstNonce + L"," + serverNonce + L"," + serverNonce;
    for (size_t slash = 0; (slash = authText.find(L'/', slash)) != std::wstring::npos; slash += 6) authText.replace(slash, 1, L"&#x2F;");
    const std::string authMessage = ToUtf8(authText);
    const std::vector<BYTE> clientSignature = HmacSha256(Bytes(authMessage), storedKey);
    if (clientKey.size() != 32 || clientSignature.size() != 32) { error = L"无法计算 CPE 挑战摘要。"; return false; }
    std::vector<BYTE> proof(32);
    for (size_t index = 0; index < proof.size(); ++index) proof[index] = clientKey[index] ^ clientSignature[index];
    const std::wstring authenticationXml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><request><clientproof>" + Hex(proof) + L"</clientproof><finalnonce>" + EscapeXml(serverNonce) + L"</finalnonce><loginflag>2</loginflag></request>";
    const Response authentication = Request(L"POST", L"/api/user/authentication_login", authenticationXml, NextVerificationToken(true), L"application/x-www-form-urlencoded; charset=UTF-8");
    if (authentication.networkError != ERROR_SUCCESS) {
        error = L"CPE 验证登录请求未送达（网络错误 " + std::to_wstring(authentication.networkError) + L"）。";
        return false;
    }
    const std::wstring code = XmlValue(authentication.body, L"code");
    if (!code.empty() || (XmlValue(authentication.body, L"response") != L"OK" && XmlValue(authentication.body, L"serversignature").empty())) {
        error = L"CPE 挑战验证失败" + (code.empty() ? L"。" : L"（错误码 " + code + L"）。");
        return false;
    }
    m_authenticated = true;
    m_usedChallengeLogin = true;
    return true;
}

bool CpeProtocol::Login(const std::wstring& address, const std::wstring& password, std::wstring& error) {
    std::wstring scramError;
    // Lock-frequency permissions are granted only to the SCRAM session on
    // this firmware.  A one-time token can occasionally be consumed while
    // opening the session, so retry SCRAM once before falling back to the
    // lower-privilege password_type=4 login.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!Open(address, error) || !AcquireSession(error)) return false;
        if (LoginChallenge(password, scramError)) return true;
        if (attempt == 0) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Older firmware only supports password_type=4. Reacquire the session
    // because a rejected SCRAM attempt consumes its one-time token.
    if (!Open(address, error) || !AcquireSession(error)) return false;
    if (LoginType4(password, error)) return true;
    if (!scramError.empty()) error += L"（SCRAM：" + scramError + L"）";
    return false;
}

bool CpeProtocol::FetchSignals(CpeSignalData& signals, std::wstring& error) {
    signals = {};
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登陆。"; return false; }
    const Response status = Request(L"GET", L"/api/monitoring/status");
    const Response signal = Request(L"GET", L"/api/device/signal");
    const Response netAccess = Request(L"GET", L"/api/device/net-access-info");
    const Response cell = Request(L"GET", L"/api/net/cell-info");
    const Response secondary = Request(L"GET", L"/api/device/seccellinfo");
    const Response neighbors = Request(L"GET", L"/api/device/nbrcellinfo");
    const Response traffic = Request(L"GET", L"/api/monitoring/traffic-statistics");
    const Response plmn = Request(L"GET", L"/api/net/current-plmn");
    auto value = [&](const wchar_t* name) {
        const std::wstring fromSignal = XmlValue(signal.body, name);
        if (!fromSignal.empty()) return fromSignal;
        const std::wstring fromCell = XmlValue(cell.body, name);
        if (!fromCell.empty()) return fromCell;
        return XmlValue(status.body, name);
    };
    signals.rsrp = FirstXmlValue(signal.body, { L"nrrsrp", L"rsrp" });
    signals.rsrq = FirstXmlValue(signal.body, { L"nrrsrq", L"rsrq" });
    signals.rssi = FirstXmlValue(signal.body, { L"nrrssi", L"rssi" });
    signals.sinr = FirstXmlValue(signal.body, { L"nrsinr", L"sinr" });
    if (signals.rsrp.empty()) signals.rsrp = value(L"rsrp");
    if (signals.rsrq.empty()) signals.rsrq = value(L"rsrq");
    if (signals.rssi.empty()) signals.rssi = value(L"rssi");
    if (signals.sinr.empty()) signals.sinr = value(L"sinr");
    signals.pccArfcn = FirstXmlValue(signal.body, { L"nrearfcn", L"earfcn" });
    signals.pccPci = XmlValue(signal.body, L"pci");
    signals.pccBandwidth = FirstXmlValue(signal.body, { L"nrdlbandwidth", L"nrulbandwidth", L"dlbandwidth" });
    const std::wstring bandInfo = FirstXmlValue(signal.body, { L"bandInfo", L"bandinfo" }) + L" " + XmlValue(signal.body, L"band");
    std::wsmatch bandMatch;
    if (std::regex_search(bandInfo, bandMatch, std::wregex(L"N\\s*(\\d+)", std::regex_constants::icase)))
        signals.pccBand = bandMatch[1].str();
    else if (std::regex_match(bandInfo, bandMatch, std::wregex(L"\\s*(\\d+)\\s*")))
        signals.pccBand = bandMatch[1].str();
    if (signals.pccBandwidth.empty() && std::regex_search(bandInfo, bandMatch, std::wregex(L"([0-9.]+\\s*MHz)", std::regex_constants::icase)))
        signals.pccBandwidth = bandMatch[1].str();
    signals.downloadRate = XmlValue(traffic.body, L"CurrentDownloadRate");
    signals.uploadRate = XmlValue(traffic.body, L"CurrentUploadRate");
    signals.currentUpload = XmlValue(traffic.body, L"CurrentUpload");
    signals.currentDownload = XmlValue(traffic.body, L"CurrentDownload");
    signals.totalUpload = XmlValue(traffic.body, L"TotalUpload");
    signals.totalDownload = XmlValue(traffic.body, L"TotalDownload");
    signals.currentConnectTime = XmlValue(traffic.body, L"CurrentConnectTime");
    signals.totalConnectTime = XmlValue(traffic.body, L"TotalConnectTime");
    signals.operatorName = XmlValue(plmn.body, L"FullName");
    const std::wstring numeric = XmlValue(plmn.body, L"Numeric");
    signals.plmn = numeric;
    signals.cellId = FirstXmlValue(signal.body, { L"cell_id", L"CellId", L"cellid", L"CELL_ID", L"nei_cellid", L"enodeb_id" });
    signals.mcsUp = FirstXmlValue(signal.body, { L"NRmcsUpCarrier1", L"nrmcsupcarrier1", L"nrmcsup", L"nrulmcs" });
    signals.mcsDown = FirstXmlValue(signal.body, { L"NRmcsDownCarrier1", L"nrmcsdowncarrier1", L"nrmcsdown", L"nrdlmcs" });
    signals.rank = FirstXmlValue(signal.body, { L"NRRank", L"nrrank", L"rank" });
    const std::wstring cqi0 = FirstXmlValue(signal.body, { L"NRCQI", L"nrcqi", L"nrcqi0", L"cqi0", L"cqi" });
    const std::wstring cqi1 = FirstXmlValue(signal.body, { L"nrcqi1", L"cqi1" });
    signals.cqi = cqi0.empty() ? cqi1 : cqi1.empty() ? cqi0 : L"CQI0:" + cqi0 + L" CQI1:" + cqi1;
    signals.accessCode = FirstXmlValue(netAccess.body, { L"net_access_code", L"NetAccessCode", L"access_code" });
    if (numeric == L"46003" || numeric == L"46005" || numeric == L"46011") signals.operatorName = L"中国电信";
    else if (numeric == L"46000" || numeric == L"46002" || numeric == L"46004" || numeric == L"46007" || numeric == L"46008") signals.operatorName = L"中国移动";
    else if (numeric == L"46001" || numeric == L"46006" || numeric == L"46009") signals.operatorName = L"中国联通";
    const std::wstring network = XmlValue(status.body, L"CurrentNetworkTypeEx");
    const std::wstring nrList = XmlValue(secondary.body, L"nrseccell_list");
    PopulateSecondaryCarrier(nrList, signals);
    signals.bandSummary = signals.pccBandwidth + L"@" + signals.pccArfcn + (signals.pccBand.empty() ? L"" : L"(N" + signals.pccBand + L")");
    if (signals.hasScc) signals.bandSummary += L" + " + signals.sccBandwidth + L"@" + signals.sccArfcn + (signals.sccBand.empty() ? L"" : L"(N" + signals.sccBand + L")");
    // ponytail: Huawei reports 5G-A as ordinary 5G; an NR secondary carrier is the usable CA signal.
    signals.networkType = network.find(L"5G-A") != std::wstring::npos || network.find(L"5GA") != std::wstring::npos || signals.hasScc ? L"5G-A" : network.find(L"4G") != std::wstring::npos || network.find(L"LTE") != std::wstring::npos ? L"4G" : L"5G";
    if (signals.rsrp.empty() && signals.rsrq.empty() && signals.rssi.empty() && signals.sinr.empty()) {
        const std::wstring code = FirstXmlValue(signal.body, { L"code" });
        error = !code.empty() ? L"CPE 信号接口返回错误码 " + code + L"。" : L"CPE 未返回小区信号数据。";
        return false;
    }
    return true;
}

bool CpeProtocol::FetchNeighborCells(CpeNeighborData& neighbors, std::wstring& error) {
    neighbors = {};
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登陆。"; return false; }
    const Response response = Request(L"GET", L"/api/device/nbrcellinfo");
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || !code.empty()) {
        error = !code.empty() ? L"CPE 邻区接口返回错误码 " + code + L"。" : L"无法读取 CPE 邻区信息。";
        return false;
    }
    neighbors.lteList = XmlValue(response.body, L"nbrcell_ltelist");
    neighbors.nrList = XmlValue(response.body, L"nbrcell_nrlist");
    return true;
}

bool CpeProtocol::FetchContractRates(CpeContractRateData& rates, std::wstring& error) {
    rates = {};
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登陆。"; return false; }

    const std::wstring enableXml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><request><enable>1</enable></request>";
    const Response enabled = Request(L"POST", L"/api/developer/atport-status", enableXml, NextVerificationToken());
    const std::wstring enableCode = XmlValue(enabled.body, L"code");
    if (enabled.networkError != ERROR_SUCCESS || enabled.status != 200 || (!enableCode.empty() && enableCode != L"0")) {
        error = L"无法开启 CPE AT 端口" + (enableCode.empty() ? L"。" : L"（错误码 " + enableCode + L"）。");
        return false;
    }

    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) { error = L"无法初始化 TCP。"; return false; }
    SOCKET socketHandle = INVALID_SOCKET;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string host = ToUtf8(m_host);
    if (getaddrinfo(host.c_str(), "20249", &hints, &addresses) == 0) {
        for (addrinfo* address = addresses; address; address = address->ai_next) {
            socketHandle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socketHandle == INVALID_SOCKET) continue;
            DWORD timeout = 1000;
            setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            if (connect(socketHandle, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) break;
            closesocket(socketHandle);
            socketHandle = INVALID_SOCKET;
        }
        freeaddrinfo(addresses);
    }
    if (socketHandle == INVALID_SOCKET) {
        WSACleanup();
        error = L"AT 端口已开启，但无法连接 CPE TCP 端口 20249。";
        return false;
    }

    auto switchData = [&](bool on) {
        const std::wstring xml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><request><dataswitch>" + std::wstring(on ? L"1" : L"0") + L"</dataswitch></request>";
        const Response response = Request(L"POST", L"/api/dialup/mobile-dataswitch", xml, NextVerificationToken());
        const std::wstring code = XmlValue(response.body, L"code");
        return response.networkError == ERROR_SUCCESS && response.status == 200 && (code.empty() || code == L"0");
    };

    if (!switchData(false)) {
        closesocket(socketHandle);
        WSACleanup();
        error = L"关闭移动数据失败，未执行签约速率读取。";
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    if (!switchData(true)) {
        closesocket(socketHandle);
        WSACleanup();
        error = L"恢复移动数据失败，请在 CPE 管理页重新开启移动数据。";
        return false;
    }

    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    char buffer[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        const int count = recv(socketHandle, buffer, sizeof(buffer), 0);
        if (count > 0) {
            received.append(buffer, buffer + count);
            const size_t marker = received.find("^DSAMBR:");
            if (marker != std::string::npos) {
                size_t end = received.find_first_of("\r\n", marker);
                if (end == std::string::npos) end = received.size();
                const std::wstring payload = ToWide(received.substr(marker + 8, end - marker - 8));
                const std::vector<std::wstring> fields = Split(payload, L',');
                if (fields.size() >= 3 && !fields[1].empty() && !fields[2].empty()) {
                    // ^DSAMBR reports: PDP context, downlink AMBR, uplink AMBR.
                    // Values are kbit/s; the UI contract-rate formatter expects bit/s.
                    try {
                        rates.downlinkRate = std::to_wstring(std::stoull(fields[1]) * 1000ULL);
                        rates.uplinkRate = std::to_wstring(std::stoull(fields[2]) * 1000ULL);
                    } catch (...) {
                        rates.downlinkRate = fields[1];
                        rates.uplinkRate = fields[2];
                    }
                    closesocket(socketHandle);
                    WSACleanup();
                    return true;
                }
            }
        } else if (count == 0) {
            break;
        }
    }
    closesocket(socketHandle);
    WSACleanup();
    error = L"等待 CPE 返回 ^DSAMBR 签约信息超时，请确认当前为 5G SA 网络后重试。";
    return false;
}

namespace {
std::wstring XmlSection(const std::wstring& xml, const std::wstring& name) {
    const std::wstring open = L"<" + name + L">";
    const std::wstring close = L"</" + name + L">";
    const size_t begin = xml.find(open);
    const size_t end = begin == std::wstring::npos ? begin : xml.find(close, begin + open.size());
    return begin == std::wstring::npos || end == std::wstring::npos ? L"" : xml.substr(begin + open.size(), end - begin - open.size());
}

CpeLockTarget ParseLockTarget(const std::wstring& xml, const wchar_t* sectionName) {
    CpeLockTarget target;
    const std::wstring section = XmlSection(xml, sectionName);
    const std::wstring firstFrequency = XmlSection(XmlSection(section, L"freq_infos"), L"freq_info");
    target.mode = XmlValue(section, L"lock_mode");
    target.band = XmlValue(firstFrequency, L"band");
    target.arfcn = XmlValue(firstFrequency, L"freq");
    target.pci = XmlValue(firstFrequency, L"pci");
    target.allBands = XmlValue(section, L"all_bands");
    return target;
}

std::wstring JoinBands(const std::vector<std::wstring>& bands) {
    std::wstring result;
    for (const std::wstring& band : bands) {
        if (!result.empty()) result += L",";
        result += band;
    }
    return result;
}

std::wstring LockInfoXml(const wchar_t* name, const std::vector<std::wstring>& bands,
                         const std::wstring& arfcn = L"", const std::wstring& pci = L"") {
    const int mode = bands.empty() ? 0 : arfcn.empty() ? 3 : pci.empty() ? 1 : 2;
    std::wstring xml = L"<" + std::wstring(name) + L"><lock_mode>" + std::to_wstring(mode) + L"</lock_mode><freq_infos>";
    for (const std::wstring& band : bands) {
        xml += L"<freq_info><band>" + EscapeXml(band) + L"</band>";
        if (!arfcn.empty()) xml += L"<freq>" + EscapeXml(arfcn) + L"</freq>";
        if (!pci.empty()) xml += L"<pci>" + EscapeXml(pci) + L"</pci>";
        xml += L"</freq_info>";
    }
    return xml + L"</freq_infos><all_bands>" + EscapeXml(JoinBands(bands)) + L"</all_bands></" + name + L">";
}
}

bool CpeProtocol::FetchLockState(CpeLockState& state, std::wstring& error) {
    state = {};
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登陆。"; return false; }
    const Response response = Request(L"GET", L"/api/net/lock-freq");
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || !code.empty()) {
        error = code == L"100003" ? L"CPE 拒绝锁频接口访问（100003：无权限）。" :
            !code.empty() ? L"CPE 锁频接口返回错误码 " + code + L"。" : L"无法读取 CPE 锁频状态。";
        return false;
    }
    state.lte = ParseLockTarget(response.body, L"lte_info");
    state.nr = ParseLockTarget(response.body, L"nr_info");
    return true;
}

bool CpeProtocol::ApplyFrequencyLock(const std::wstring& body, std::wstring& error) {
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登陆。"; return false; }
    const std::wstring xml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><request>" + body + L"</request>";
    const Response response = Request(L"POST", L"/api/net/lock-freq", xml, NextVerificationToken(),
                                      L"application/x-www-form-urlencoded; charset=UTF-8");
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || (!code.empty() && code != L"0") ||
        (!XmlValue(response.body, L"response").empty() && XmlValue(response.body, L"response") != L"OK")) {
        error = code == L"100003" ? L"CPE 拒绝锁频操作（100003：无权限）。" :
            code == L"125003" ? L"CPE 锁频会话令牌已过期，请重新连接后重试。" :
            !code.empty() ? L"CPE 锁频失败（错误码 " + code + L"）。" : L"CPE 锁频请求失败。";
        return false;
    }
    return true;
}

bool CpeProtocol::SetBandLock(const std::vector<std::wstring>& lteBands,
                              const std::vector<std::wstring>& nrBands, std::wstring& error) {
    if (lteBands.empty() && nrBands.empty()) { error = L"请至少选择一个 4G 或 5G 频段。"; return false; }
    return ApplyFrequencyLock(LockInfoXml(L"lte_info", lteBands) + LockInfoXml(L"nr_info", nrBands), error);
}

bool CpeProtocol::SetCellLock(bool nr, const std::wstring& band, const std::wstring& arfcn,
                              const std::wstring& pci, std::wstring& error) {
    if (band.empty() || arfcn.empty() || pci.empty()) { error = L"频段、频点和 PCI 均为必填项。"; return false; }
    const std::vector<std::wstring> selected{ band };
    const std::wstring unlocked = LockInfoXml(nr ? L"lte_info" : L"nr_info", {});
    const std::wstring locked = LockInfoXml(nr ? L"nr_info" : L"lte_info", selected, arfcn, pci);
    return ApplyFrequencyLock(nr ? unlocked + locked : locked + unlocked, error);
}

bool CpeProtocol::ClearFrequencyLock(std::wstring& error) {
    // LTE and NR locks are both stored by /api/net/lock-freq.  The legacy
    // net-mode endpoint only resets LTE masks, leaving an NR lock active (and
    // returns -1 on firmware that does not expose that endpoint).
    return ApplyFrequencyLock(LockInfoXml(L"lte_info", {}) + LockInfoXml(L"nr_info", {}), error);
}

bool CpeProtocol::FetchNetworkMode(CpeNetworkMode& mode, std::wstring& error) {
    mode = {};
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登录。"; return false; }
    const Response response = Request(L"GET", L"/api/net/net-mode");
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || !code.empty()) {
        error = !code.empty() ? L"读取网络方式失败（错误码 " + code + L"）。" : L"无法读取 CPE 网络方式。";
        return false;
    }
    mode.mode = XmlValue(response.body, L"NetworkMode");
    mode.networkBand = XmlValue(response.body, L"NetworkBand");
    mode.lteBand = XmlValue(response.body, L"LTEBand");
    mode.networkOption = XmlValue(response.body, L"networkOption");
    if (mode.mode.empty() || mode.networkBand.empty() || mode.lteBand.empty()) {
        error = L"CPE 未返回完整网络方式配置。";
        return false;
    }
    return true;
}

static bool ParseAccelerationRecordsByState(const std::wstring& response, std::vector<CpeTerminalDevice>& devices, bool activeRecords) {
    devices.clear();
    bool foundStatus = false;
    const std::wregex objectExpression(L"\\{([^{}]*)\\}");
    const auto field = [](const std::wstring& object, const wchar_t* name) {
        std::wsmatch match;
        const std::wregex expression(L"\\\"" + std::wstring(name) + L"\\\"\\s*:\\s*(?:\\\"([^\\\"]*)\\\"|([^,}\\s]+))", std::regex_constants::icase);
        if (!std::regex_search(object, match, expression)) return std::wstring{};
        return match[1].matched ? match[1].str() : match[2].str();
    };
    const auto isActive = [](std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(), towlower);
        return value == L"true" || value == L"1" || value == L"accelerating";
    };
    for (std::wsregex_iterator it(response.begin(), response.end(), objectExpression), end; it != end; ++it) {
        const std::wstring object = (*it)[1].str();
        const std::wstring status = field(object, L"AclrStatus");
        if (status.empty()) continue;
        foundStatus = true;
        std::wstring mac = field(object, L"MacAddr");
        if (mac.empty()) mac = field(object, L"MacAddress");
        if (isActive(status) != activeRecords || mac.empty()) continue;
        const std::wstring totalDuration = field(object, L"TotalAclrTime");
        if (!activeRecords && (totalDuration.empty() || totalDuration == L"0")) continue;
        std::wstring duration = totalDuration;
        const std::wstring start = field(object, L"StartAclrTime");
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (activeRecords && swscanf_s(start.c_str(), L"%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
            std::tm started{};
            started.tm_year = year - 1900;
            started.tm_mon = month - 1;
            started.tm_mday = day;
            started.tm_hour = hour;
            started.tm_min = minute;
            started.tm_sec = second;
            started.tm_isdst = -1;
            const std::time_t startTime = std::mktime(&started);
            const std::time_t now = std::time(nullptr);
            if (startTime != static_cast<std::time_t>(-1) && now >= startTime) duration = std::to_wstring(now - startTime);
            else duration = L"0";
        }
        devices.push_back({ mac, mac, duration.empty() ? L"accelerating" : duration, totalDuration });
    }
    return foundStatus;
}

bool CpeProtocol::ParseAccelerationRecords(const std::wstring& response, std::vector<CpeTerminalDevice>& devices) {
    return ParseAccelerationRecordsByState(response, devices, true);
}

bool CpeProtocol::ParseAccelerationHistoryRecords(const std::wstring& response, std::vector<CpeTerminalDevice>& devices) {
    return ParseAccelerationRecordsByState(response, devices, false);
}

bool CpeProtocol::FetchConnectedDevices(std::vector<CpeTerminalDevice>& devices, std::wstring& error) {
    devices.clear();
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登录。"; return false; }
    // The App Turbo list is the source of truth. The terminal list below only
    // resolves returned MAC addresses to the friendly device name.
    const Response response = Request(L"GET", L"/api/appturbo/acc-list");
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || (!code.empty() && code != L"0")) {
        error = !code.empty() ? L"读取加速设备失败（错误码 " + code + L"）。" : L"无法读取 CPE 加速设备。";
        return false;
    }
    const bool jsonRecords = ParseAccelerationRecords(response.body, devices);
    auto normalizeMac = [](std::wstring value) {
        std::wstring normalized;
        for (const wchar_t character : value) if (iswxdigit(character)) normalized += static_cast<wchar_t>(towupper(character));
        return normalized;
    };
    const std::wregex hostExpression(L"<(Host|Device|Item|AccDevice|DeviceInfo|Deviceinfo|AccInfo)>([\\s\\S]*?)</\\1>", std::regex_constants::icase);
    if (!jsonRecords) for (std::wsregex_iterator it(response.body.begin(), response.body.end(), hostExpression), end; it != end; ++it) {
        const std::wstring host = (*it)[2].str();
        CpeTerminalDevice device;
        device.name = FirstXmlValue(host, { L"HostName", L"hostname", L"DeviceName", L"devicename", L"Name", L"name" });
        device.mac = FirstXmlValue(host, { L"MacAddress", L"macaddress", L"MACAddress", L"MacAddr", L"macaddr", L"mac", L"Mac", L"mac_addr", L"mac_address", L"DeviceMac", L"devicemac" });
        device.status = FirstXmlValue(host, { L"AccelerateTime", L"accelerate_time", L"Duration", L"duration", L"Status", L"status" });
        if (device.name.empty()) device.name = device.mac.empty() ? L"未知设备" : device.mac;
        if (!device.mac.empty()) devices.push_back(std::move(device));
    }
    // App Turbo firmware variants may return a flat MAC list instead of one
    // XML object per terminal. Collect those values and resolve them through
    // the normal terminal inventory, but never treat every terminal as active.
    const std::wregex macValue(L"(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}");
    std::vector<std::wstring> acceleratedMacs;
    if (!jsonRecords) for (std::wsregex_iterator it(response.body.begin(), response.body.end(), macValue), end; it != end; ++it) {
        const std::wstring mac = (*it).str();
        if (std::none_of(acceleratedMacs.begin(), acceleratedMacs.end(), [&](const std::wstring& current) { return normalizeMac(current) == normalizeMac(mac); })) acceleratedMacs.push_back(mac);
    }
    for (const CpeTerminalDevice& device : devices) {
        if (std::none_of(acceleratedMacs.begin(), acceleratedMacs.end(), [&](const std::wstring& current) { return normalizeMac(current) == normalizeMac(device.mac); })) acceleratedMacs.push_back(device.mac);
    }
    if (!acceleratedMacs.empty()) {
        const Response terminals = Request(L"GET", L"/api/wlan/host-list");
        if (terminals.networkError == ERROR_SUCCESS && terminals.status == 200 && XmlValue(terminals.body, L"code").empty()) {
            const std::wregex terminalExpression(L"<(Host|Device|Item|DeviceInfo|Deviceinfo)>([\\s\\S]*?)</\\1>", std::regex_constants::icase);
            for (std::wsregex_iterator it(terminals.body.begin(), terminals.body.end(), terminalExpression), end; it != end; ++it) {
                const std::wstring terminal = (*it)[2].str();
                const std::wstring mac = FirstXmlValue(terminal, { L"MacAddress", L"macaddress", L"MACAddress", L"MacAddr", L"macaddr", L"mac", L"Mac", L"mac_addr", L"mac_address" });
                const auto active = std::find_if(acceleratedMacs.begin(), acceleratedMacs.end(), [&](const std::wstring& value) { return normalizeMac(value) == normalizeMac(mac); });
                if (active == acceleratedMacs.end()) continue;
                const std::wstring name = FirstXmlValue(terminal, { L"HostName", L"hostname", L"DeviceName", L"devicename", L"Name", L"name" });
                const auto current = std::find_if(devices.begin(), devices.end(), [&](const CpeTerminalDevice& device) { return normalizeMac(device.mac) == normalizeMac(mac); });
                if (current == devices.end()) devices.push_back({ name.empty() ? *active : name, *active, L"accelerating" });
                else if (current->name.empty() || normalizeMac(current->name) == normalizeMac(current->mac)) current->name = name.empty() ? *active : name;
            }
        }
    }

    // Some App Turbo firmwares flatten the acceleration list instead of
    // wrapping each device in a Host/Device node.  In that format the MAC is
    // still the stable identifier shown by the official UI.
    // Preserve valid records even if the friendly-name inventory is unavailable.
    if (!jsonRecords) for (const std::wstring& mac : acceleratedMacs) {
        if (std::none_of(devices.begin(), devices.end(), [&](const CpeTerminalDevice& device) { return normalizeMac(device.mac) == normalizeMac(mac); })) devices.push_back({ mac, mac, L"accelerating" });
    }
    if (!jsonRecords && devices.empty()) {
        const std::wregex macExpression(L"<(MacAddress|macaddress|MACAddress|mac|mac_addr)>([^<]+)</\\1>", std::regex_constants::icase);
        for (std::wsregex_iterator it(response.body.begin(), response.body.end(), macExpression), end; it != end; ++it) {
            CpeTerminalDevice device;
            device.mac = (*it)[2].str();
            device.name = device.mac;
            device.status = L"加速中";
            devices.push_back(std::move(device));
        }
    }
    if (!jsonRecords && devices.empty()) {
        error = L"CPE 加速列表未返回设备记录。";
        return false;
    }
    return true;
}

bool CpeProtocol::FetchAccelerationHistory(std::vector<CpeTerminalDevice>& devices, std::wstring& error) {
    devices.clear();
    if (!IsConnected()) { error = L"CPE session has expired."; return false; }
    const Response response = Request(L"GET", L"/api/appturbo/acc-list");
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || (!code.empty() && code != L"0")) {
        error = !code.empty() ? L"Failed to read CPE acceleration history (code " + code + L")." : L"Unable to read CPE acceleration history.";
        return false;
    }
    const bool jsonRecords = ParseAccelerationHistoryRecords(response.body, devices);
    if (!jsonRecords) {
        error = L"CPE acceleration history response has an unsupported format.";
        return false;
    }

    return true;
}

bool CpeProtocol::FetchDeviceInformation(CpeDeviceInformation& information, std::wstring& error) {
    information = {};
    if (!IsConnected()) { error = L"CPE session has expired."; return false; }
    const Response response = Request(L"GET", L"/api/device/information");
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || (!code.empty() && code != L"0")) {
        error = !code.empty() ? L"Failed to read CPE device information (code " + code + L")." : L"Unable to read CPE device information.";
        return false;
    }
    information.model = FirstXmlValue(response.body, { L"DeviceName", L"DeviceType", L"ProductName", L"Model" });
    information.uptime = FirstXmlValue(response.body, { L"Uptime", L"uptime", L"UpTime", L"RunTime" });
    information.serialNumber = FirstXmlValue(response.body, { L"SerialNumber", L"SerialNum", L"Sn", L"SN" });
    information.imei = FirstXmlValue(response.body, { L"Imei", L"IMEI", L"Imei1" });
    information.imsi = FirstXmlValue(response.body, { L"Imsi", L"IMSI" });
    information.phoneNumber = FirstXmlValue(response.body, { L"Msisdn", L"MSISDN", L"PhoneNumber", L"Phone" });
    information.hardwareVersion = FirstXmlValue(response.body, { L"HardwareVersion", L"HardwareVer" });
    information.softwareVersion = FirstXmlValue(response.body, { L"SoftwareVersion", L"SoftwareVer" });
    information.webUiVersion = FirstXmlValue(response.body, { L"WebUIVersion", L"WebUiVersion", L"WebUIVer" });
    information.configVersion = FirstXmlValue(response.body, { L"ConfigVersion", L"ConfigurationVersion", L"ConfigurationFileVersion", L"ConfigVer", L"CfgVersion", L"CustomizeVersion", L"iniversion" });
    information.parameterVersion = FirstXmlValue(response.body, { L"ParameterVersion", L"ParamVersion" });
    if (information.model.empty() && information.serialNumber.empty() && information.imei.empty()) {
        error = L"CPE device information response is incomplete.";
        return false;
    }
    return true;
}

bool CpeProtocol::FetchAvailableDevices(std::vector<CpeTerminalDevice>& devices, std::wstring& error) {
    devices.clear();
    if (!IsConnected()) { error = L"CPE session has expired."; return false; }
    const Response response = Request(L"GET", L"/api/wlan/host-list");
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || (!code.empty() && code != L"0")) {
        error = !code.empty() ? L"Failed to read CPE terminal list (code " + code + L")." : L"Unable to read CPE terminal list.";
        return false;
    }
    const std::wregex terminalExpression(L"<(Host|Device|Item|DeviceInfo|Deviceinfo)>([\\s\\S]*?)</\\1>", std::regex_constants::icase);
    for (std::wsregex_iterator it(response.body.begin(), response.body.end(), terminalExpression), end; it != end; ++it) {
        const std::wstring terminal = (*it)[2].str();
        const std::wstring mac = FirstXmlValue(terminal, { L"MacAddress", L"macaddress", L"MACAddress", L"MacAddr", L"macaddr", L"mac", L"Mac", L"mac_addr", L"mac_address" });
        if (mac.empty()) continue;
        const std::wstring name = FirstXmlValue(terminal, { L"HostName", L"hostname", L"DeviceName", L"devicename", L"Name", L"name" });
        const std::wstring status = FirstXmlValue(terminal, { L"Status", L"status", L"Active", L"active" });
        devices.push_back({ name.empty() ? mac : name, mac, status });
    }
    return true;
}

bool CpeProtocol::SetNetworkMode(const CpeNetworkMode& mode, std::wstring& error) {
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登录。"; return false; }
    if (mode.mode.empty() || mode.networkBand.empty() || mode.lteBand.empty()) {
        error = L"网络方式参数不完整。";
        return false;
    }
    const std::wstring xml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><request><NetworkMode>" +
        EscapeXml(mode.mode) + L"</NetworkMode><NetworkBand>" + EscapeXml(mode.networkBand) +
        L"</NetworkBand><LTEBand>" + EscapeXml(mode.lteBand) + L"</LTEBand><networkOption>" +
        EscapeXml(mode.networkOption) + L"</networkOption></request>";
    const Response response = Request(L"POST", L"/api/net/net-mode", xml, NextVerificationToken());
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || (!code.empty() && code != L"0")) {
        error = !code.empty() ? L"设置网络方式失败（错误码 " + code + L"）。" : L"CPE 未接受网络方式设置。";
        return false;
    }
    return true;
}

bool CpeProtocol::SetMobileData(bool enabled, std::wstring& error) {
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登录。"; return false; }
    const std::wstring xml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><request><dataswitch>" +
        std::wstring(enabled ? L"1" : L"0") + L"</dataswitch></request>";
    const Response response = Request(L"POST", L"/api/dialup/mobile-dataswitch", xml, NextVerificationToken());
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || (!code.empty() && code != L"0")) {
        error = !code.empty() ? L"设置移动数据失败（错误码 " + code + L"）。" : L"CPE 未接受移动数据设置。";
        return false;
    }
    return true;
}

bool CpeProtocol::FetchSmsMessages(bool sent, std::vector<CpeSmsMessage>& messages,
                                   size_t& totalCount, std::wstring& error) {
    messages.clear();
    totalCount = 0;
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登录。"; return false; }

    const std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><request>"
        L"<PageIndex>1</PageIndex><ReadCount>50</ReadCount><BoxType>" +
        std::wstring(sent ? L"2" : L"1") +
        L"</BoxType><SortType>0</SortType><Ascending>0</Ascending>"
        L"<UnreadPreferred>0</UnreadPreferred></request>";
    const Response response = Request(L"POST", L"/api/sms/sms-list", xml, NextVerificationToken(),
                                      L"application/x-www-form-urlencoded; charset=UTF-8");
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || !code.empty()) {
        error = response.networkError != ERROR_SUCCESS
            ? L"短信列表请求未送达（网络错误 " + std::to_wstring(response.networkError) + L"）。"
            : !code.empty() ? L"CPE 读取短信失败（错误码 " + code + L"）。"
                            : L"CPE 短信接口 HTTP " + std::to_wstring(response.status) + L"。";
        return false;
    }

    size_t messageStart = 0;
    while ((messageStart = response.body.find(L"<Message>", messageStart)) != std::wstring::npos) {
        const size_t contentStart = messageStart + 9;
        const size_t messageEnd = response.body.find(L"</Message>", contentStart);
        if (messageEnd == std::wstring::npos) break;
        const std::wstring block = response.body.substr(contentStart, messageEnd - contentStart);
        CpeSmsMessage message;
        message.index = XmlValue(block, L"Index");
        message.phone = UnescapeXml(XmlValue(block, L"Phone"));
        message.content = UnescapeXml(XmlValue(block, L"Content"));
        message.date = UnescapeXml(XmlValue(block, L"Date"));
        message.unread = XmlValue(block, L"Smstat") == L"0";
        if (!message.index.empty() || !message.phone.empty() || !message.content.empty()) messages.push_back(std::move(message));
        messageStart = messageEnd + 10;
    }
    const std::wstring count = XmlValue(response.body, L"Count");
    try { totalCount = count.empty() ? messages.size() : std::stoull(count); }
    catch (...) { totalCount = messages.size(); }
    return true;
}

bool CpeProtocol::FetchSmsCounts(CpeSmsCounts& counts, std::wstring& error) {
    counts = {};
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登录。"; return false; }
    const Response response = Request(L"GET", L"/api/sms/sms-count");
    const std::wstring code = XmlValue(response.body, L"code");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || !code.empty()) {
        error = response.networkError != ERROR_SUCCESS
            ? L"SIM 短信数量请求未送达（网络错误 " + std::to_wstring(response.networkError) + L"）。"
            : !code.empty() ? L"CPE 读取 SIM 短信数量失败（错误码 " + code + L"）。"
                            : L"CPE 短信数量接口 HTTP " + std::to_wstring(response.status) + L"。";
        return false;
    }
    const auto count = [&](const wchar_t* name) {
        try { return static_cast<size_t>(std::stoull(XmlValue(response.body, name))); }
        catch (...) { return size_t{ 0 }; }
    };
    counts.localInbox = count(L"LocalInbox");
    counts.localOutbox = count(L"LocalOutbox");
    counts.simInbox = count(L"SimInbox");
    counts.simOutbox = count(L"SimOutbox");
    return true;
}

bool CpeProtocol::SendSms(const std::wstring& phone, const std::wstring& content, std::wstring& error) {
    if (!IsConnected()) { error = L"CPE 会话已失效，请重新登录。"; return false; }
    if (phone.empty() || content.empty()) { error = L"收件人号码和短信内容不能为空。"; return false; }

    const std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><request><Index>-1</Index><Phones><Phone>" +
        EscapeXml(phone) + L"</Phone></Phones><Sca></Sca><Content>" + EscapeXml(content) +
        L"</Content><Length>" + std::to_wstring(content.size()) +
        L"</Length><Reserved>1</Reserved><Date>-1</Date></request>";
    const Response response = Request(L"POST", L"/api/sms/send-sms", xml, NextVerificationToken(),
                                      L"application/x-www-form-urlencoded; charset=UTF-8");
    const std::wstring code = XmlValue(response.body, L"code");
    const std::wstring result = XmlValue(response.body, L"response");
    if (response.networkError != ERROR_SUCCESS || response.status != 200 || !code.empty() ||
        (!result.empty() && result != L"OK")) {
        error = response.networkError != ERROR_SUCCESS
            ? L"短信发送请求未送达（网络错误 " + std::to_wstring(response.networkError) + L"）。"
            : !code.empty() ? L"CPE 发送短信失败（错误码 " + code + L"）。"
                            : L"CPE 未接受短信发送请求。";
        return false;
    }
    return true;
}
