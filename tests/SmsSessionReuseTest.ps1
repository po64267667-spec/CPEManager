$ErrorActionPreference = 'Stop'

$main = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main.cpp')
$match = [regex]::Match($main, 'static std::wstring SmsListJson\(bool sent\) \{(?<body>[\s\S]*?)\n\}')
if (-not $match.Success) { throw 'Could not locate SmsListJson.' }
$body = $match.Groups['body'].Value

if ($body -match 'if \(!ReconnectSavedCpe\(error\) \|\|') {
    throw 'SMS refresh unconditionally logs in again even when the current CPE session is valid.'
}
if ($body -notmatch '!g_cpeProtocol\.IsConnected\(\)\s*&&\s*!ReconnectSavedCpe\(error\)') {
    throw 'SMS refresh does not explicitly reuse an existing authenticated CPE session.'
}

Write-Output 'SMS session reuse regression test passed.'

$sendMatch = [regex]::Match($main, 'static std::wstring SmsSendJson\([^\)]*\) \{(?<body>[\s\S]*?)\n\}')
if (-not $sendMatch.Success) { throw 'Could not locate SmsSendJson.' }
$sendBody = $sendMatch.Groups['body'].Value
if ($sendBody -match 'if \(!ReconnectSavedCpe\(error\) \|\|') {
    throw 'SMS send unconditionally logs in again even when the current CPE session is valid.'
}
if ($sendBody -notmatch '!g_cpeProtocol\.IsConnected\(\)\s*&&\s*!ReconnectSavedCpe\(error\)') {
    throw 'SMS send does not explicitly reuse an existing authenticated CPE session.'
}
