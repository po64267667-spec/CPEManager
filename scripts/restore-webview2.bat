@echo off
setlocal
set "PACKAGE=packages\webview2"
if exist "%PACKAGE%\build\native\include\WebView2.h" exit /b 0
echo Downloading Microsoft.Web.WebView2 1.0.2903.40...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $package='packages/webview2'; New-Item -ItemType Directory -Force -Path $package | Out-Null; Invoke-WebRequest 'https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40' -OutFile 'packages/webview2.nupkg'; Expand-Archive -Force 'packages/webview2.nupkg' $package"
