#include <fstream>
#include <iostream>
#include <locale>
#include <string>
#include <vector>

#include "../CpeProtocol.h"

std::wstring Value(const std::wstring& text, const std::wstring& key) {
    const size_t begin = text.find(key + L"=");
    if (begin == std::wstring::npos) return L"";
    const size_t valueBegin = begin + key.size() + 1;
    const size_t end = text.find_first_of(L"\r\n", valueBegin);
    return text.substr(valueBegin, end == std::wstring::npos ? std::wstring::npos : end - valueBegin);
}

int main() {
    std::wifstream input(L"..\\cpe_login.txt");
    input.imbue(std::locale(""));
    const std::wstring saved((std::istreambuf_iterator<wchar_t>(input)), std::istreambuf_iterator<wchar_t>());
    CpeProtocol protocol;
    std::wstring error;
    if (!protocol.Login(Value(saved, L"url"), Value(saved, L"password"), error)) {
        std::wcerr << L"Login failed: " << error << L"\n";
        return 2;
    }
    std::vector<CpeSmsMessage> messages;
    size_t inboxCount = 0, outboxCount = 0;
    if (!protocol.FetchSmsMessages(false, messages, inboxCount, error)) {
        std::wcerr << L"Inbox failed: " << error << L"\n";
        return 3;
    }
    const size_t inboxLoaded = messages.size();
    CpeSmsCounts simCounts;
    if (!protocol.FetchSmsCounts(simCounts, error)) {
        std::wcerr << L"SIM count failed: " << error << L"\n";
        return 4;
    }
    if (!protocol.FetchSmsMessages(true, messages, outboxCount, error)) {
        std::wcerr << L"Outbox failed: " << error << L"\n";
        return 5;
    }
    std::wcout << L"SMS boxes read succeeded in one session; inbox storage=" << simCounts.localInbox + simCounts.simInbox
               << L" (local=" << simCounts.localInbox << L", SIM=" << simCounts.simInbox << L")"
               << L"; inbox list total=" << inboxCount << L", loaded=" << inboxLoaded
               << L"; outbox total=" << outboxCount << L", loaded=" << messages.size() << L"\n";
    return 0;
}
