$html = Get-Content -Raw -LiteralPath 'index.html'
$protocol = Get-Content -Raw -LiteralPath 'CpeProtocol.cpp'
$failures = @()

if ($html -match "homeFiveGCell\('pcc','PCC'\)") {
    $failures += 'Lock page still injects the home PCC unconditionally.'
}
$homeBand = [regex]::Match($html, 'homeBand=(?<body>.*?),homeBandwidth=').Groups['body'].Value
if ($homeBand -match "bands\.includes\('78'\)" -or $html -match "cell\.band==='N78'") {
    $failures += 'Front end still forces N78 to take priority.'
}
if ($protocol -notmatch 'fields\[0\] == signals\.pccArfcn && fields\[3\] == signals\.pccPci\) continue') {
    $failures += 'Protocol still accepts an SCC that duplicates the PCC.'
}
if ($protocol -notmatch 'signals\.networkType[^;]+signals\.hasScc') {
    $failures += 'Network type still trusts the raw secondary-cell list.'
}

if ($failures.Count) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Output 'SCC visibility regression test passed.'
