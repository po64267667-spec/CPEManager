$ErrorActionPreference = 'Stop'

$edge = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
$index = (Resolve-Path (Join-Path $PSScriptRoot '..\index.html')).Path
$port = 9342
$profile = Join-Path ([IO.Path]::GetTempPath()) ('cpe-sms-test-' + [guid]::NewGuid().ToString('N'))
$process = $null
$socket = $null
$nextId = 0

function Invoke-Cdp([string]$method, $params) {
    $script:nextId++
    $request = @{ id = $script:nextId; method = $method; params = $params } | ConvertTo-Json -Compress -Depth 8
    $bytes = [Text.Encoding]::UTF8.GetBytes($request)
    [void]$script:socket.SendAsync([ArraySegment[byte]]::new($bytes), [Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    do {
        $buffer = New-Object byte[] 65536
        $builder = [Text.StringBuilder]::new()
        do {
            $received = $script:socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), [Threading.CancellationToken]::None).GetAwaiter().GetResult()
            [void]$builder.Append([Text.Encoding]::UTF8.GetString($buffer, 0, $received.Count))
        } while (-not $received.EndOfMessage)
        $response = $builder.ToString() | ConvertFrom-Json
    } while ($response.id -ne $script:nextId)
    return $response
}

function Eval([string]$expression) {
    return (Invoke-Cdp 'Runtime.evaluate' @{ expression = $expression; returnByValue = $true }).result.result.value
}

try {
    New-Item -ItemType Directory -Path $profile | Out-Null
    $process = Start-Process -FilePath $edge -ArgumentList @('--headless=new', '--disable-gpu', "--remote-debugging-port=$port", "--user-data-dir=$profile", 'about:blank') -WindowStyle Hidden -PassThru
    $targets = $null
    for ($attempt = 0; $attempt -lt 40 -and -not $targets; $attempt++) {
        try { $targets = Invoke-RestMethod -Uri "http://127.0.0.1:$port/json" -TimeoutSec 1 } catch { Start-Sleep -Milliseconds 100 }
    }
    $target = $targets | Where-Object type -eq 'page' | Select-Object -First 1
    if (-not $target) { throw 'Could not find a DevTools page target.' }
    $socket = [Net.WebSockets.ClientWebSocket]::new()
    [void]$socket.ConnectAsync([uri]$target.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    [void](Invoke-Cdp 'Page.enable' @{})

    $mock = @'
window.__posted = [];
window.CPE_CONTROL_TIMEOUT_MS = 60;
const mockWebView = new EventTarget();
mockWebView.postMessage = value => window.__posted.push(JSON.parse(value));
if (!window.chrome) Object.defineProperty(window, 'chrome', { value: {}, configurable: true });
Object.defineProperty(window.chrome, 'webview', { value: mockWebView, configurable: true });
window.__emitCpe = data => mockWebView.dispatchEvent(new MessageEvent('message', { data }));
'@
    [void](Invoke-Cdp 'Page.addScriptToEvaluateOnNewDocument' @{ source = $mock })
    $accelerationUi = Get-Content -Raw (Join-Path $PSScriptRoot '..\acceleration-ui.js')
    [void](Invoke-Cdp 'Page.addScriptToEvaluateOnNewDocument' @{ source = $accelerationUi })
    [void](Invoke-Cdp 'Page.navigate' @{ url = ([uri]$index).AbsoluteUri })
    for ($attempt = 0; $attempt -lt 30 -and -not (Eval "typeof requestSmsBox === 'function'"); $attempt++) { Start-Sleep -Milliseconds 100 }
    if (-not (Eval "typeof requestSmsBox === 'function'")) { throw 'SMS UI did not initialize.' }
    if (-not (Eval 'Boolean(bridge)')) { throw 'Mock WebView bridge did not initialize.' }
    if (-not (Eval 'window.__posted.some(x=>x.action==="refresh-acceleration-devices")')) { throw 'Application acceleration data was not requested during initialization.' }
    if (-not (Eval 'document.querySelectorAll("#control .accel-record").length===0')) { throw 'Configured placeholder devices must not be shown as actively accelerating.' }
    [void](Eval 'window.__emitCpe({action:"cpe-acceleration-devices",status:"ok",devices:[]})')
    if (-not (Eval 'document.querySelectorAll("#control .accel-record").length===0')) { throw 'An empty active CPE response must leave the active list empty.' }
    [void](Eval 'window.__emitCpe({action:"cpe-acceleration-devices",status:"ok",devices:[{id:"desktop",name:"DESKTOP-GBHU52B",detail:"online",duration:"120"}],availableDevices:[{id:"desktop",name:"DESKTOP-GBHU52B",detail:"online"},{id:"phone",name:"PHONE",detail:"online"},{id:"tablet",name:"TABLET",detail:"online"},{id:"tv",name:"TV",detail:"online"},{id:"console",name:"CONSOLE",detail:"online"},{id:"pad",name:"PAD",detail:"online"}]})')
    if (-not (Eval 'document.querySelectorAll("#control .accel-record").length===1&&document.querySelector("#control .accel-record b").textContent==="DESKTOP-GBHU52B"')) { throw 'All-device acceleration must show only the current CPE acceleration target.' }
    if (-not (Eval 'document.querySelector("#control .accel-record span:last-child").textContent.startsWith("2")')) { throw 'Acceleration duration must come from the CPE record, not a status label.' }
    [void](Eval 'document.querySelectorAll("#control .accel-tabs [role=tab]")[1].click()')
    if (-not (Eval 'JSON.parse(localStorage.getItem("cpe-accel-history")||"[]").length===0')) { throw 'An active acceleration session must not appear in history.' }
    if (-not (Eval 'document.querySelectorAll("#control .accel-record").length===0')) { throw 'History must not show a still-active acceleration session.' }
    [void](Eval 'window.__emitCpe({action:"cpe-acceleration-devices",status:"ok",devices:[],availableDevices:[]})')
    if (-not (Eval 'JSON.parse(localStorage.getItem("cpe-accel-history")||"[]").length===1')) { throw 'A finished acceleration session was not archived.' }
    if (-not (Eval 'document.querySelectorAll("#control .accel-record").length===1&&document.querySelector("#control .accel-record b").textContent==="DESKTOP-GBHU52B"')) { throw 'Finished acceleration session was not shown in history.' }
    [void](Eval 'document.querySelectorAll("#control .accel-tabs [role=tab]")[0].click()')
    if (-not (Eval 'document.querySelectorAll("#control .control-switch").length===2')) { throw 'Control page switches were not rendered.' }
    if (Eval 'Boolean(document.querySelector("#control .turbo-options"))') { throw 'Turbo controls were not removed.' }
    if (Eval 'document.getElementById("control").textContent.includes("IPv6")||document.getElementById("control").textContent.includes("NFC")||document.getElementById("control").textContent.includes("VPN")') { throw 'Removed control options are still visible.' }
    [void](Eval 'window.__posted=[]; document.querySelector("#control .control-switch").click()')
    if (-not (Eval 'window.__posted.some(x=>x.action==="set-mobile-data"&&x.enabled===false)')) { throw 'Mobile data setting was not posted.' }
    [void](Eval 'window.__emitCpe({action:"cpe-control",status:"ok",control:"mobile-data",enabled:false,message:"saved"})')
    if (Eval 'document.querySelector("#control .control-switch").classList.contains("on")') { throw 'Mobile data switch did not apply the CPE response.' }
    [void](Eval 'window.__posted=[]; document.querySelector("#control .control-switch").click()')
    if (-not (Eval 'window.__posted.some(x=>x.action==="set-mobile-data"&&x.enabled===true)')) { throw 'Mobile data enable request was not posted.' }
    [void](Eval 'window.__emitCpe({action:"cpe-control",status:"ok",control:"mobile-data",enabled:true,message:"saved"})')
    if (-not (Eval 'document.querySelector("#control .control-switch").classList.contains("on")')) { throw 'Mobile data switch did not re-enable.' }
    [void](Eval 'window.__posted=[]; document.querySelectorAll("#control .control-segment")[0].querySelectorAll("button")[2].click()')
    if (-not (Eval 'window.__posted.some(x=>x.action==="set-network-control"&&x.preference==="4g")')) { throw 'Network preference was not posted.' }
    [void](Eval 'window.__emitCpe({action:"cpe-control",status:"ok",control:"network",networkMode:"03",networkOption:"1",message:"saved"})')
    if (-not (Eval 'document.querySelector("#control .control-status").textContent==="\u6b63\u5728\u8bbe\u7f6e\u4e2d\uff0c\u7b49\u5f85\u7f51\u7edc\u6062\u590d\u3002"')) { throw 'Network recovery feedback was not shown.' }
    if (-not (Eval 'document.querySelectorAll("#control .control-segment")[0].querySelector("button").disabled')) { throw 'Network controls were enabled before the network recovered.' }
    [void](Eval 'window.__posted=[]; window.__emitCpe({action:"cpe-data",status:"ok"})')
    if (-not (Eval 'window.__posted.some(x=>x.action==="refresh-control")')) { throw 'Control status was not refreshed after network recovery.' }
    [void](Eval 'window.__emitCpe({action:"cpe-control",status:"ok",networkMode:"03",networkOption:"1"})')
    if (-not (Eval 'document.querySelectorAll("#control .control-segment")[0].querySelectorAll("button")[2].classList.contains("active")')) { throw 'Network preference did not apply the CPE response.' }
    if (-not (Eval 'document.querySelector("#control .control-status").textContent===""')) { throw 'Network recovery feedback was not cleared after the control status refresh.' }
    [void](Eval 'document.querySelectorAll("#control .control-segment")[0].querySelectorAll("button")[1].click()')
    Start-Sleep -Milliseconds 150
    if (Eval 'document.querySelector("#control .control-status").textContent.includes("\u4fdd\u5b58")') { throw 'Network control remains stuck in the saving state when the CPE does not respond.' }
    [void](Eval 'document.querySelectorAll("#control .control-switch")[1].click()')
    if (Eval 'document.querySelectorAll("#control .control-switch")[1].classList.contains("on")') { throw 'Application acceleration switch did not toggle.' }
    if (-not (Eval 'getComputedStyle(document.querySelector("#control .control-switch")).height==="26px"&&getComputedStyle(document.querySelector("#control .control-segment")).height==="32px"&&getComputedStyle(document.querySelector("#control .accel-select-wrap")).height==="48px"&&getComputedStyle(document.querySelector("#control .accel-dropdown-trigger")).height==="30px"')) { throw 'Control button sizes are incorrect.' }
    if (-not (Eval 'document.querySelectorAll("#control .accel-dropdown").length===2&&document.querySelectorAll("#control .accel-dropdown-trigger").length===2')) { throw 'Application acceleration selectors are not lock-frequency style dropdowns.' }
    if (-not (Eval '[...document.querySelectorAll("#control .accel-select-label")].every(x=>x.textContent.trim().length===4)')) { throw 'Application acceleration dropdown labels are missing.' }
    if (-not (Eval '[...document.querySelectorAll("#control .accel-dropdown-menu button")].some(x=>x.textContent==="\u6307\u5b9a")&&!document.getElementById("control").textContent.includes("\u5f53\u524d\u8bbe\u5907")')) { throw 'The device selector was not renamed to designated.' }
    [void](Eval 'document.querySelector("#control .accel-dropdown-trigger").click()')
    if (-not (Eval 'document.querySelector("#control .accel-dropdown").classList.contains("open")')) { throw 'Application acceleration dropdown did not open.' }
    [void](Eval 'document.querySelector("#control .accel-dropdown-menu button").click()')
    if (-not (Eval 'document.querySelector("#control .accel-dropdown").classList.contains("open")')) { throw 'Unsupported CPE scene selection should not claim success.' }
    [void](Eval 'document.querySelectorAll("#control .accel-dropdown-trigger")[1].click();document.querySelectorAll("#control .accel-dropdown-menu")[1].querySelectorAll("button")[1].click()')
    if (-not (Eval '(()=>{const picker=document.querySelector("#control .accel-device-picker");return !picker.hidden&&picker.querySelectorAll(".accel-device-option").length===6&&getComputedStyle(picker).maxHeight==="229px"})()')) { throw 'Designated-device picker did not use the full CPE terminal list.' }
    [void](Eval 'document.querySelectorAll("#control .accel-device-option")[0].click();document.querySelectorAll("#control .accel-device-option")[4].click()')
    if (-not (Eval 'document.querySelectorAll("#control .accel-device-option.selected").length===2&&localStorage.getItem("cpe-accel-devices").split(",").length===2')) { throw 'CPE terminal devices could not be multi-selected.' }
    [void](Eval 'document.querySelectorAll("#control .control-switch")[1].click()')
    if (-not (Eval 'document.querySelectorAll("#control .accel-record").length===0')) { throw 'Ended devices must not return to the active list.' }
    [void](Eval 'window.__posted=[]; document.querySelector("[data-page=\"control\"]").click()')
    if (-not (Eval 'window.__posted.some(x=>x.action==="refresh-acceleration-devices")')) { throw 'CPE device list was not requested for application acceleration.' }
    [void](Eval 'document.querySelectorAll("#control .control-switch")[1].click();document.querySelectorAll("#control .accel-tabs [role=tab]")[1].click()')
    if (-not (Eval 'document.querySelectorAll("#control .accel-tabs [role=tab]")[1].classList.contains("active")&&document.querySelectorAll("#control .accel-record").length===1')) { throw 'Acceleration history tab did not show archived devices.' }

    if (-not (Eval 'document.getElementById("sms-compose-body").hidden')) { throw 'Compose form must be collapsed initially.' }
    [void](Eval 'document.getElementById("sms-compose-toggle").click()')
    if (Eval 'document.getElementById("sms-compose-body").hidden') { throw 'Compose form did not expand.' }
    if (-not (Eval 'document.querySelector("#sms-compose-toggle span").textContent==="\u6536\u8d77"')) { throw 'Compose toggle label did not change to collapse.' }
    if (-not (Eval 'document.getElementById("sms-compose-toggle").classList.contains("expanded")')) { throw 'Compose arrow did not switch upward.' }
    [void](Eval 'document.getElementById("sms-compose-toggle").click()')
    if (-not (Eval 'document.getElementById("sms-compose-body").hidden')) { throw 'Compose form did not collapse.' }

    [void](Eval 'window.__posted=[]; document.querySelectorAll("#sms-box-select .sms-select-menu button")[1].click()')
    if (-not (Eval 'document.getElementById("sms-inbox").hidden && !document.getElementById("sms-outbox").hidden')) { throw 'Outbox switch failed.' }
    if (-not (Eval 'window.__posted.some(x=>x.action==="refresh-sms"&&x.box==="outbox")')) { throw ('Outbox refresh was not requested: ' + (Eval 'JSON.stringify(window.__posted)')) }

    [void](Eval 'window.__emitCpe({action:"cpe-sms-list",status:"ok",box:"outbox",totalCount:7,messages:[{phone:"10086",content:"full message body",date:"2026-08-21",unread:false}]})')
    if ((Eval 'document.querySelector("#sms-outbox .sms-message-phone").textContent') -ne '10086') { throw 'SMS list rendering failed.' }
    if ((Eval 'document.getElementById("sms-count").textContent') -ne '7') { throw 'Outbox total count was not displayed.' }
    if (-not (Eval 'Boolean(document.querySelector("#sms-outbox .sms-message-icon.outbox svg"))')) { throw 'Outbox send icon was not rendered.' }
    [void](Eval 'document.querySelector("#sms-outbox .sms-message").click()')
    if (-not (Eval 'document.getElementById("sms-detail").open')) { throw 'SMS detail dialog did not open.' }
    if ((Eval 'document.getElementById("sms-detail-content").textContent') -ne 'full message body') { throw 'SMS detail content was not complete.' }
    if (-not (Eval 'document.getElementById("sms-detail-icon").classList.contains("outbox")')) { throw 'SMS detail did not show the send icon.' }
    [void](Eval 'document.getElementById("sms-detail-close").click()')

    [void](Eval 'document.querySelectorAll("#sms-box-select .sms-select-menu button")[0].click(); window.__emitCpe({action:"cpe-sms-list",status:"ok",box:"inbox",totalCount:3,messages:[{phone:"10000",content:"inbox message",date:"2026-08-22",unread:true}]})')
    if (-not (Eval 'Boolean(document.querySelector("#sms-inbox .sms-message-icon.inbox svg"))')) { throw 'Inbox receive icon was not rendered.' }
    if ((Eval 'document.getElementById("sms-count").textContent') -ne '3') { throw 'Inbox SIM count was not displayed.' }
    [void](Eval 'window.__emitCpe({action:"cpe-data",status:"error",error:"disconnected"})')
    if (Eval 'Boolean(document.querySelector("#sms-inbox .sms-message"))') { throw 'Inbox content remained visible after CPE disconnect.' }
    if (-not (Eval 'document.getElementById("sms-count").textContent==="\u2014"')) { throw 'SMS count remained visible after CPE disconnect.' }

    [void](Eval 'const content=document.getElementById("sms-content"); content.value="hello"; content.dispatchEvent(new Event("input",{bubbles:true}))')
    if (-not (Eval 'document.getElementById("sms-character-count").textContent==="5 / 1000"')) { throw 'SMS character counter did not update.' }
    if (-not (Eval 'getComputedStyle(document.getElementById("sms-content")).resize==="none"')) { throw 'SMS compose textarea remains resizable.' }
    if (-not (Eval 'document.getElementById("sms-send").style.width==="50%"')) { throw 'SMS send button width was not reduced.' }
    [void](Eval 'window.__posted=[]; document.getElementById("sms-phone").value="13800138000"; document.getElementById("sms-send").click()')
    if (-not (Eval 'window.__posted.some(x=>x.action==="send-sms"&&x.phone==="13800138000"&&x.content==="hello")')) { throw 'SMS send request was not posted.' }
    [void](Eval 'window.__emitCpe({action:"cpe-sms-send",status:"ok",message:"sent"})')
    if (-not (Eval 'document.getElementById("sms-send-status").textContent==="sent"&&!document.getElementById("sms-send-status").classList.contains("error")')) { throw 'SMS send success feedback was not shown.' }
    if (-not (Eval 'document.getElementById("sms-character-count").textContent==="0 / 1000"')) { throw 'SMS character counter was not reset after sending.' }
    [void](Eval 'window.__emitCpe({action:"cpe-sms-send",status:"error",error:"failed"})')
    if (-not (Eval 'document.getElementById("sms-send-status").textContent==="failed"&&document.getElementById("sms-send-status").classList.contains("error")')) { throw 'SMS send error feedback was not shown.' }
    Write-Output 'SMS UI regression test passed.'
}
finally {
    if ($socket) { $socket.Dispose() }
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -like "*$profile*" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $profile) {
        $resolved = [IO.Path]::GetFullPath($profile)
        $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or -not ([IO.Path]::GetFileName($resolved)).StartsWith('cpe-sms-test-')) { throw "Refusing to remove unexpected test profile: $resolved" }
        for ($attempt = 0; $attempt -lt 20 -and (Test-Path -LiteralPath $resolved); $attempt++) {
            try { Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 100 }
        }
    }
}
