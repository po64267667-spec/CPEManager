#define wWinMain CpeManagerApplicationEntryPoint
#include "../main.cpp"
#undef wWinMain

int main() {
    if (JsonValue(L"{\"action\":\"set-mobile-data\",\"enabled\":true}", L"enabled") != L"true") return 1;
    if (JsonValue(L"{\"action\":\"set-mobile-data\",\"enabled\":false}", L"enabled") != L"false") return 1;
    return 0;
}
