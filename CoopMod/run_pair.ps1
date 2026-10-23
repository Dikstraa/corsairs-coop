# Starts a host and a client game instance on this PC and shows coopnet.log.
#
# The engine refuses a second instance while a window titled "Pirates of the Maelstrom" exists,
# so the client runs from Maelstrom_dev2.exe: a local copy with that title changed (never committed).
# During a session CoopNet.dll keeps both windows running even when they are not active.
# -TestSkip: the host sleeps 8 hours in game, the client agrees to the vote by itself.
param([int]$Seconds = 60, [switch]$NewGame, [switch]$TestSkip, [switch]$Keep)

$game = Split-Path $PSScriptRoot -Parent
$dev2 = Join-Path $game 'Maelstrom_dev2.exe'
if (-not (Test-Path $dev2)) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $game 'Maelstrom.exe'))
    $title = [Text.Encoding]::ASCII.GetBytes('Pirates of the Maelstrom')
    $at = -1
    for ($i = 0; $i -le $bytes.Length - $title.Length -and $at -lt 0; $i++) {
        $j = 0
        while ($j -lt $title.Length -and $bytes[$i + $j] -eq $title[$j]) { $j++ }
        if ($j -eq $title.Length) { $at = $i }
    }
    if ($at -lt 0) { throw 'window title not found in Maelstrom.exe' }
    $bytes[$at + $title.Length - 1] = [byte][char]'2'
    [IO.File]::WriteAllBytes($dev2, $bytes)
    Write-Host "created $dev2"
}

Get-Process Maelstrom, Maelstrom_dev2 -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item (Join-Path $game 'coopnet.log'), (Join-Path $game 'error.log') -ErrorAction SilentlyContinue

if ($NewGame) { $env:COOPNET_NEWGAME = '1' } # both instances press "New game" in the main menu
$env:COOPNET_TEST = 'host'
if ($TestSkip) { $env:COOPNET_TESTSKIP = '1' }
$hostProc = Start-Process (Join-Path $game 'Maelstrom.exe') -WorkingDirectory $game -PassThru
Start-Sleep 8
$env:COOPNET_TEST = 'client'
Remove-Item Env:COOPNET_TESTSKIP -ErrorAction SilentlyContinue
if ($TestSkip) { $env:COOPNET_AUTOVOTE = '1' }
$clientProc = Start-Process $dev2 -WorkingDirectory $game -PassThru
Remove-Item Env:COOPNET_TEST, Env:COOPNET_NEWGAME, Env:COOPNET_AUTOVOTE -ErrorAction SilentlyContinue

Write-Host "host pid $($hostProc.Id), client pid $($clientProc.Id); waiting $Seconds s (switch between the windows)"
Start-Sleep $Seconds
Get-Content (Join-Path $game 'coopnet.log')
if (Test-Path (Join-Path $game 'error.log')) {
    Write-Host '--- error.log (unique lines)'
    Get-Content (Join-Path $game 'error.log') -Encoding Default | Sort-Object -Unique
}
if (-not $Keep) { $hostProc, $clientProc | Stop-Process -Force -ErrorAction SilentlyContinue }
