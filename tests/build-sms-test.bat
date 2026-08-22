@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /utf-8 /std:c++17 /EHsc CpeProtocolSmsTest.cpp ..\CpeProtocol.cpp /link /out:CpeProtocolSmsTest.exe
cl /nologo /utf-8 /std:c++17 /EHsc CpeSmsLiveProbe.cpp ..\CpeProtocol.cpp /link /out:CpeSmsLiveProbe.exe
