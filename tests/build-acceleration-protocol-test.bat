@echo off
pushd "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /utf-8 /std:c++17 /EHsc CpeProtocolAccelerationTest.cpp ..\CpeProtocol.cpp /link /out:CpeProtocolAccelerationTest.exe
set "test_result=%ERRORLEVEL%"
popd
exit /b %test_result%
