$ErrorActionPreference = 'Stop'

$edge = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
$index = (Resolve-Path (Join-Path $PSScriptRoot '..\index.html')).Path
$port = 9341
$profile = Join-Path ([IO.Path]::GetTempPath()) ('cpe-reconnect-test-' + [guid]::NewGuid().ToString('N'))
$process = $null
$socket = $null
$nextId = 0

function Invoke-Cdp([string]$method, $params) {
    $script:nextId++
    $request = @{ id = $script:nextId; method = $method; params = $params } | ConvertTo-Json -Compress -Depth 8
    $bytes = [Text.Encoding]::UTF8.GetBytes($request)
    [void]$script:socket.SendAsync([ArraySegment[byte]]::new($bytes), [Net.WebSockets.WebSocketMessageType]::Text,
        $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    do {
        $buffer = New-Object byte[] 65536
        $builder = [Text.StringBuilder]::new()
        do {
            $received = $script:socket.ReceiveAsync([ArraySegment[byte]]::new($buffer),
                [Threading.CancellationToken]::None).GetAwaiter().GetResult()
            [void]$builder.Append([Text.Encoding]::UTF8.GetString($buffer, 0, $received.Count))
        } while (-not $received.EndOfMessage)
        $response = $builder.ToString() | ConvertFrom-Json
    } while ($response.id -ne $script:nextId)
    return $response
}

try {
    New-Item -ItemType Directory -Path $profile | Out-Null
    $process = Start-Process -FilePath $edge -ArgumentList @('--headless=new', '--disable-gpu',
        "--remote-debugging-port=$port", "--user-data-dir=$profile", 'about:blank') -WindowStyle Hidden -PassThru
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
const mockWebView = new EventTarget();
mockWebView.postMessage = value => window.__posted.push(JSON.parse(value));
if (!window.chrome) Object.defineProperty(window, 'chrome', { value: {}, configurable: true });
Object.defineProperty(window.chrome, 'webview', { value: mockWebView, configurable: true });
window.__emitCpe = data => mockWebView.dispatchEvent(new MessageEvent('message', { data }));
'@
    [void](Invoke-Cdp 'Page.addScriptToEvaluateOnNewDocument' @{ source = $mock })
    [void](Invoke-Cdp 'Page.navigate' @{ url = ([uri]$index).AbsoluteUri })
    $isReady = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        $ready = Invoke-Cdp 'Runtime.evaluate' @{ expression = "typeof window.__emitCpe === 'function' && typeof renderNeighbors === 'function'"; returnByValue = $true }
        if ($ready.result.result.value) { $isReady = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $isReady) {
        $diagnostic = Invoke-Cdp 'Runtime.evaluate' @{ expression = "location.href+' | emit='+typeof window.__emitCpe+' | timer='+typeof cpeRefreshTimer"; returnByValue = $true }
        throw "Test setup failed: $($diagnostic.result.result.value)"
    }

    [void](Invoke-Cdp 'Runtime.evaluate' @{ expression = "window.__emitCpe({action:'cpe-data',status:'ok',refresh:false,networkType:'5G'}); window.__posted=[]"; returnByValue = $true })
    Start-Sleep -Milliseconds 1200
    $baseline = Invoke-Cdp 'Runtime.evaluate' @{ expression = "window.__posted.filter(x=>x.action==='refresh-cpe').length"; returnByValue = $true }
    if ($baseline.result.result.value -lt 1) { throw 'Test setup failed: normal refresh timer did not run.' }

    [void](Invoke-Cdp 'Runtime.evaluate' @{ expression = "window.__posted=[]; window.__emitCpe({action:'cpe-data',status:'error',refresh:true,error:'temporary disconnect'})"; returnByValue = $true })
    Start-Sleep -Milliseconds 3300
    $afterError = Invoke-Cdp 'Runtime.evaluate' @{ expression = "window.__posted.filter(x=>x.action==='refresh-cpe').length"; returnByValue = $true }
    if ($afterError.result.result.value -lt 1) { throw 'Reconnect failed: refresh polling stopped after a temporary CPE error.' }
    Write-Output 'Unlock reconnect UI regression test passed.'
}
finally {
    if ($socket) { $socket.Dispose() }
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" |
        Where-Object { $_.CommandLine -like "*$profile*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $profile) {
        $resolved = [IO.Path]::GetFullPath($profile)
        $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            -not ([IO.Path]::GetFileName($resolved)).StartsWith('cpe-reconnect-test-')) {
            throw "Refusing to remove unexpected test profile: $resolved"
        }
        for ($attempt = 0; $attempt -lt 20 -and (Test-Path -LiteralPath $resolved); $attempt++) {
            try { Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 100 }
        }
        if (Test-Path -LiteralPath $resolved) { throw "Could not remove test profile: $resolved" }
    }
}
