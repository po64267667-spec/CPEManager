@echo off
pushd "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /utf-8 /std:c++17 /EHsc /I ..\packages\webview2\build\native\include ControlMessageParsingTest.cpp ..\CpeProtocol.cpp /link ..\packages\webview2\build\native\x64\WebView2LoaderStatic.lib User32.lib Advapi32.lib Ole32.lib /out:ControlMessageParsingTest.exe
set "control_test_result=%ERRORLEVEL%"
popd
exit /b %control_test_result%
