#include <cassert>
#include <string>
#include <thread>
#include <vector>

#define private public
#include "../CpeProtocol.h"
#undef private

namespace {
struct Request { std::string method, path, body, raw; };

std::string ReadRequest(SOCKET socket) {
    std::string request;
    char buffer[2048];
    size_t bodyStart = std::string::npos, contentLength = 0;
    while (true) {
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        assert(received > 0);
        request.append(buffer, received);
        if (bodyStart == std::string::npos) {
            const size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd == std::string::npos) continue;
            bodyStart = headerEnd + 4;
            const std::string marker = "Content-Length: ";
            const size_t start = request.find(marker);
            if (start != std::string::npos) contentLength = std::stoul(request.substr(start + marker.size()));
        }
        if (request.size() >= bodyStart + contentLength) return request;
    }
}

Request ParseRequest(const std::string& raw) {
    const size_t firstSpace = raw.find(' '), secondSpace = raw.find(' ', firstSpace + 1);
    const size_t bodyStart = raw.find("\r\n\r\n") + 4;
    return { raw.substr(0, firstSpace), raw.substr(firstSpace + 1, secondSpace - firstSpace - 1), raw.substr(bodyStart), raw };
}

void Reply(SOCKET socket, const std::string& body) {
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    assert(send(socket, response.data(), static_cast<int>(response.size()), 0) == static_cast<int>(response.size()));
}
}

int main() {
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    assert(listen(listener, 3) == 0);
    int addressLength = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0);

    std::vector<Request> requests;
    std::thread server([&] {
        for (int index = 0; index < 3; ++index) {
            SOCKET client = accept(listener, nullptr, nullptr);
            assert(client != INVALID_SOCKET);
            requests.push_back(ParseRequest(ReadRequest(client)));
            Reply(client, index == 0
                ? "<response><Count>113</Count><Messages><Message><Smstat>0</Smstat><Index>42</Index><Phone>10001</Phone><Content>A&amp;B</Content><Date>2026-08-21 10:00:00</Date></Message></Messages></response>"
                : index == 1 ? "<response><LocalInbox>76</LocalInbox><LocalOutbox>5</LocalOutbox><SimInbox>37</SimInbox><SimOutbox>2</SimOutbox></response>"
                             : "<response>OK</response>");
            closesocket(client);
        }
    });

    CpeProtocol protocol;
    std::wstring error;
    protocol.m_session = WinHttpOpen(L"CpeProtocolSmsTest", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    assert(protocol.m_session);
    protocol.m_connection = WinHttpConnect(protocol.m_session, L"127.0.0.1", ntohs(address.sin_port), 0);
    assert(protocol.m_connection);
    protocol.m_authenticated = true;
    protocol.m_cookie = L"SessionID=test";
    protocol.m_verificationTokens = { L"sms-list-token", L"sms-send-token" };

    std::vector<CpeSmsMessage> messages;
    size_t totalCount = 0;
    assert(protocol.FetchSmsMessages(false, messages, totalCount, error));
    assert(totalCount == 113);
    assert(messages.size() == 1 && messages[0].index == L"42" && messages[0].phone == L"10001");
    assert(messages[0].content == L"A&B" && messages[0].unread);
    CpeSmsCounts counts;
    assert(protocol.FetchSmsCounts(counts, error));
    assert(counts.localInbox == 76 && counts.localOutbox == 5);
    assert(counts.simInbox == 37 && counts.simOutbox == 2);
    assert(protocol.SendSms(L"13800138000", L"A&B", error));

    server.join();
    closesocket(listener);
    WSACleanup();

    assert(requests.size() == 3);
    assert(requests[0].method == "POST" && requests[0].path == "/api/sms/sms-list");
    assert(requests[0].raw.find("__RequestVerificationToken: sms-list-token") != std::string::npos);
    assert(requests[0].body.find("<ReadCount>50</ReadCount>") != std::string::npos);
    assert(requests[0].body.find("<BoxType>1</BoxType>") != std::string::npos);
    assert(requests[1].method == "GET" && requests[1].path == "/api/sms/sms-count");
    assert(requests[2].method == "POST" && requests[2].path == "/api/sms/send-sms");
    assert(requests[2].raw.find("__RequestVerificationToken: sms-send-token") != std::string::npos);
    assert(requests[2].body.find("<Phone>13800138000</Phone>") != std::string::npos);
    assert(requests[2].body.find("<Content>A&amp;B</Content>") != std::string::npos);
    assert(requests[2].body.find("<Length>3</Length>") != std::string::npos);
    return 0;
}
