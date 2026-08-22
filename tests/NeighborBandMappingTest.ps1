$ErrorActionPreference = 'Stop'

$edge = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
$index = (Resolve-Path (Join-Path $PSScriptRoot '..\index.html')).Path
$port = 9337
$profile = Join-Path ([System.IO.Path]::GetTempPath()) ('cpe-neighbor-band-test-' + [guid]::NewGuid().ToString('N'))
$process = $null
$socket = $null

try {
    New-Item -ItemType Directory -Path $profile | Out-Null
    $uri = ([uri]$index).AbsoluteUri
    $arguments = @('--headless=new', '--disable-gpu', "--remote-debugging-port=$port", "--user-data-dir=$profile", $uri)
    $process = Start-Process -FilePath $edge -ArgumentList $arguments -WindowStyle Hidden -PassThru

    $targets = $null
    for ($attempt = 0; $attempt -lt 40 -and -not $targets; $attempt++) {
        try { $targets = Invoke-RestMethod -Uri "http://127.0.0.1:$port/json" -TimeoutSec 1 } catch { Start-Sleep -Milliseconds 100 }
    }
    $target = $targets | Where-Object { $_.type -eq 'page' -and $_.url -like '*index.html*' } | Select-Object -First 1
    if (-not $target) { throw 'Could not find the index.html DevTools target.' }

    $socket = [System.Net.WebSockets.ClientWebSocket]::new()
    [void]$socket.ConnectAsync([uri]$target.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    $expression = @"
(() => {
  const neighborCases = [
    ['627264,N77/N78,10,-90,-10,-65,10;', 'N78'],
    ['627264,N77,11,-90,-10,-65,10;', 'N78'],
    ['660000,N77,12,-90,-10,-65,10;', 'N77'],
    ['627264,N78,13,-90,-10,-65,10;', 'N78']
  ];
  const neighborResults = neighborCases.map(([raw, expected]) => {
    renderNeighbors('nr-neighbors', 'nr-neighbor-count', raw, 'N', '');
    return { actual: document.querySelector('#nr-neighbors .neighbor-id b').textContent, expected };
  });
  return neighborResults;
})()
"@
    $request = @{ id = 1; method = 'Runtime.evaluate'; params = @{ expression = $expression; returnByValue = $true } } | ConvertTo-Json -Compress -Depth 6
    $sendBytes = [Text.Encoding]::UTF8.GetBytes($request)
    [void]$socket.SendAsync([ArraySegment[byte]]::new($sendBytes), [Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()

    do {
        $receiveBytes = New-Object byte[] 65536
        $segment = [ArraySegment[byte]]::new($receiveBytes)
        $builder = [Text.StringBuilder]::new()
        do {
            $received = $socket.ReceiveAsync($segment, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
            [void]$builder.Append([Text.Encoding]::UTF8.GetString($receiveBytes, 0, $received.Count))
        } while (-not $received.EndOfMessage)
        $response = $builder.ToString() | ConvertFrom-Json
    } while ($response.id -ne 1)

    $results = $response.result.result.value
    $failures = @($results | Where-Object { $_.actual -ne $_.expected })
    if ($failures.Count) {
        $summary = ($failures | ForEach-Object { "actual=$($_.actual), expected=$($_.expected)" }) -join '; '
        throw "Neighbor band mapping failed: $summary"
    }
    Write-Output 'Neighbor band mapping regression test passed.'
}
finally {
    if ($socket) { $socket.Dispose() }
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" |
        Where-Object { $_.CommandLine -like "*$profile*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $profile) {
        $resolved = [System.IO.Path]::GetFullPath($profile)
        $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            -not ([System.IO.Path]::GetFileName($resolved)).StartsWith('cpe-neighbor-band-test-')) {
            throw "Refusing to remove unexpected test profile: $resolved"
        }
        for ($attempt = 0; $attempt -lt 20 -and (Test-Path -LiteralPath $resolved); $attempt++) {
            try { Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 100 }
        }
        if (Test-Path -LiteralPath $resolved) { throw "Could not remove test profile: $resolved" }
    }
}
