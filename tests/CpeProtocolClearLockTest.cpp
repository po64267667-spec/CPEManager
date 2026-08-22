#include <algorithm>
#include <cassert>
#include <string>
#include <thread>
#include <vector>

#define private public
#include "../CpeProtocol.h"
#undef private

namespace {
struct Request {
    std::string method;
    std::string path;
    std::string body;
    std::string raw;
};

std::string ReadRequest(SOCKET socket) {
    std::string request;
    char buffer[1024];
    size_t bodyStart = std::string::npos;
    size_t contentLength = 0;
    while (true) {
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        assert(received > 0);
        request.append(buffer, received);
        if (bodyStart == std::string::npos) {
            const size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd == std::string::npos) continue;
            bodyStart = headerEnd + 4;
            const std::string marker = "Content-Length: ";
            const size_t lengthStart = request.find(marker);
            if (lengthStart != std::string::npos)
                contentLength = static_cast<size_t>(std::stoul(request.substr(lengthStart + marker.size())));
        }
        if (bodyStart != std::string::npos && request.size() >= bodyStart + contentLength) return request;
    }
}

Request ParseRequest(const std::string& raw) {
    const size_t lineEnd = raw.find("\r\n");
    const size_t firstSpace = raw.find(' ');
    const size_t secondSpace = raw.find(' ', firstSpace + 1);
    const size_t bodyStart = raw.find("\r\n\r\n") + 4;
    return { raw.substr(0, firstSpace), raw.substr(firstSpace + 1, secondSpace - firstSpace - 1), raw.substr(bodyStart), raw };
}

void Reply(SOCKET socket, const char* body) {
    const std::string payload(body);
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: " +
        std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
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
    assert(listen(listener, 2) == 0);
    int addressLength = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0);

    std::vector<Request> requests;
    std::thread server([&] {
        for (int index = 0; index < 1; ++index) {
            SOCKET client = accept(listener, nullptr, nullptr);
            assert(client != INVALID_SOCKET);
            requests.push_back(ParseRequest(ReadRequest(client)));
            Reply(client, "<response>OK</response>");
            closesocket(client);
        }
    });

    CpeProtocol protocol;
    std::wstring error;
    protocol.m_session = WinHttpOpen(L"CpeProtocolClearLockTest", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    assert(protocol.m_session);
    protocol.m_connection = WinHttpConnect(protocol.m_session, L"127.0.0.1", ntohs(address.sin_port), 0);
    assert(protocol.m_connection);
    protocol.m_authenticated = true;
    protocol.m_cookie = L"SessionID=initial";
    protocol.m_verificationTokens = { L"scram-write-token" };
    assert(protocol.ClearFrequencyLock(error));
    server.join();
    closesocket(listener);
    WSACleanup();

    assert(requests.size() == 1);
    assert(requests[0].method == "POST" && requests[0].path == "/api/net/lock-freq");
    assert(requests[0].raw.find("__RequestVerificationToken: scram-write-token") != std::string::npos);
    assert(requests[0].body.find("<lte_info><lock_mode>0</lock_mode>") != std::string::npos);
    assert(requests[0].body.find("<nr_info><lock_mode>0</lock_mode>") != std::string::npos);
    assert(requests[0].body.find("/api/net/net-mode") == std::string::npos);
    return 0;
}
