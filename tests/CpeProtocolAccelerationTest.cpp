#include <iostream>
#include <vector>

#include "../CpeProtocol.h"

int main() {
    std::vector<CpeTerminalDevice> devices;
    if (!CpeProtocol::ParseAccelerationRecords(
            L"[{\"MacAddr\":\"AA:BB:CC:DD:EE:FF\",\"AclrStatus\":false,\"TotalAclrTime\":9}]", devices) ||
        !devices.empty()) {
        std::cerr << "Inactive acceleration record must not be reported as active.\n";
        return 1;
    }

    if (!CpeProtocol::ParseAccelerationHistoryRecords(
            L"[{\"MacAddr\":\"AA:BB:CC:DD:EE:FF\",\"AclrStatus\":false,\"TotalAclrTime\":7200}]", devices) ||
        devices.size() != 1 || devices[0].mac != L"AA:BB:CC:DD:EE:FF" || devices[0].status != L"7200" || devices[0].totalDuration != L"7200") {
        std::cerr << "Finished CPE acceleration record was not parsed as cumulative history.\n";
        return 4;
    }

    if (!CpeProtocol::ParseAccelerationRecords(
            L"[{\"MacAddr\":\"AA:BB:CC:DD:EE:FF\",\"AclrStatus\":true,\"TotalAclrTime\":120}]", devices) ||
        devices.size() != 1 || devices[0].mac != L"AA:BB:CC:DD:EE:FF" || devices[0].status != L"120" || devices[0].totalDuration != L"120") {
        std::cerr << "Active acceleration record or its duration was parsed incorrectly.\n";
        return 2;
    }

    if (!CpeProtocol::ParseAccelerationRecords(
            L"[{\"MacAddr\":\"AA:BB:CC:DD:EE:FF\",\"AclrStatus\":true,\"TotalAclrTime\":120,\"StartAclrTime\":\"2099-01-01 00:00:00\"}]", devices) ||
        devices.size() != 1 || devices[0].status != L"0") {
        std::cerr << "Current-session duration must use StartAclrTime instead of the cumulative total.\n";
        return 3;
    }

    std::cout << "Acceleration protocol parsing test passed.\n";
    return 0;
}
