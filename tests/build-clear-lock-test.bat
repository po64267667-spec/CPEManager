@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /utf-8 /std:c++17 /EHsc /I . tests\CpeProtocolClearLockTest.cpp CpeProtocol.cpp /Fe:tests\CpeProtocolClearLockTest.exe
