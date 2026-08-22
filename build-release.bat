@echo off
setlocal
pushd "%~dp0"
call scripts\restore-webview2.bat || exit /b 1
if not defined VSCMD_VER call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++17 /EHsc /I packages\webview2\build\native\include main.cpp CpeProtocol.cpp /link packages\webview2\build\native\x64\WebView2LoaderStatic.lib User32.lib Advapi32.lib Ole32.lib Crypt32.lib /out:CPEManager.exe || exit /b 1
cl /nologo /utf-8 /std:c++17 /EHsc /LD CpeContractRatePlugin.cpp CpeProtocol.cpp /link /out:CpeContractRate.dll || exit /b 1
popd
