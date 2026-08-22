#include <cassert>

#include "../CpeProtocol.cpp"

int main() {
    CpeSignalData signal;
    signal.pccArfcn = L"627264";
    signal.pccPci = L"656";
    PopulateSecondaryCarrier(
        L"627264,N78,100MHz,656,-83dBm,-10dB,-60dBm,24dB;"
        L"428910,N1,20MHz,260,-92dBm,-13dB,-66dBm,1dB;",
        signal);
    assert(signal.hasScc);
    assert(signal.sccArfcn == L"428910");
    assert(signal.sccBand == L"N1");
    assert(signal.sccBandwidth == L"20MHz");
    assert(signal.sccPci == L"260");
    return 0;
}
