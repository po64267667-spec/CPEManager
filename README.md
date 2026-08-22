# CPEManager

A native Windows desktop client for compatible Huawei CPE routers. It uses WebView2 for the interface and communicates with the router's local web API to show signal data, manage network modes and bands, view SMS, and access supported device controls.

## Requirements

- Windows 10 or later
- Visual Studio 2022 Community (or Build Tools) with the Desktop development with C++ workload
- Internet access the first time you build, so the WebView2 SDK can be downloaded from NuGet

## Build

Run `build-release.bat` from a Developer Command Prompt for Visual Studio. The script downloads `Microsoft.Web.WebView2` version `1.0.2903.40` into `packages/` when needed, then produces `CPEManager.exe` and `CpeContractRate.dll`.

## Security and privacy

- Do not commit `cpe_login.txt`, `cpe_login.dat`, `WebViewData/`, logs, test captures, or generated binaries.
- CPE credentials saved by the app are protected with Windows DPAPI and can be decrypted only by the Windows user who saved them. They are never returned to the embedded web interface.
- The tool can reveal router and SIM information, connected-device metadata, and SMS content. Use it only on a router you own or are authorized to administer.

## Tests

The `tests/` directory contains protocol and UI probes. Some tests need a reachable local CPE and valid credentials; keep all such credentials in files excluded by `.gitignore`.

## Before publishing

Choose and add a software license, then review the supported router models and API behavior before advertising broad compatibility. No license is included in this repository because the project owner must make that legal choice.
