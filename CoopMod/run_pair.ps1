# Starts a host and a client game instance on this PC and shows coopnet.log.
#
# The engine refuses a second instance while a window titled "Pirates of the Maelstrom" exists,
# so the client runs from Maelstrom_dev2.exe: a local copy with that title changed (never committed).
# During a session CoopNet.dll keeps both windows running even when they are not active.
# -TestSkip: the host sleeps 8 hours in game, the client agrees to the vote by itself.
# -ToMap: both go to the world map once in the game.
# -ToSea: both go from the world map to the sea next to each other, then the host sets sail.
# -SeaJoin: only the host goes to sea; the client stays on the map by its avatar and joins it there.
# -SeaFlags (with -ToSea or -SeaJoin): the host sails under France, the client under England with a companion.
# -TestHero: the client's hero is kept by the host; after -Seconds the client restarts and gets it back.
# -TestRules: a fake boarding of the client pauses the host; the client dies and the host rolls back.
# -TestWorld: the host and the client each change the shared world and log what they see.
# -TestLand (with -NewGame): both go to Bridgetown; the client runs about, then draws its blade.
# -TestGive, -TestSleep (with -TestLand): the client gives the host money and potions by F4; the client sleeps till morning, the host agrees.
# -TestStory (with -TestLand): the host kills the tavern waitress once in the tavern; the client checks she died in its game too.
# -TestTalk (with -TestLand): no fights; the host talks to a citizen, the client logs what the host's puppet and the people near it do.
# -TestSwing (with -TestLand): no fights; the client stands in front of the host with a blade and swings every 1.5 s.
param([int]$Seconds = 60, [switch]$NewGame, [switch]$TestSkip, [switch]$ToMap, [switch]$TestWorld, [switch]$ToSea, [switch]$SeaJoin, [switch]$SeaFlags, [switch]$TestHero, [switch]$TestRules, [switch]$TestLand, [switch]$TestGive, [switch]$TestSleep, [switch]$TestStory, [switch]$TestTalk, [switch]$TestSwing, [switch]$Keep)

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

# Two full-screen instances can't share one screen: both start windowed, start.ini is restored after.
$startIni = Join-Path $game 'start.ini'
$startText = [IO.File]::ReadAllText($startIni)
[IO.File]::WriteAllText($startIni, ($startText -replace 'full_screen = 1', 'full_screen = 0'))
# a real game's log is kept as *.prev.log, not thrown away: it may be the one to look into
foreach ($name in 'coopnet', 'error') {
    $old = Join-Path $game "$name.log"
    if (Test-Path $old) { Move-Item $old (Join-Path $game "$name.prev.log") -Force }
}

if ($NewGame) { $env:COOPNET_NEWGAME = '1' }
if ($ToMap) { $env:COOPNET_TOMAP = '1' } # both instances press "New game" in the main menu
if ($TestWorld) { $env:COOPNET_TESTWORLD = '1' }
if ($ToSea -or $SeaJoin) { $env:COOPNET_TOMAP = '1'; $env:COOPNET_TOSEA = '1' }
if ($SeaJoin) { $env:COOPNET_SEAJOIN = '1' }
if ($SeaFlags) { $env:COOPNET_SEAFLAGS = '1' }
if ($TestRules) { $env:COOPNET_RULESTEST = '1' } # both instances
if ($TestLand) { $env:COOPNET_LANDTEST = '1' } # both instances
if ($TestSwing) { $env:COOPNET_SWINGTEST = '1' } # the client swings in front of the host's camera
if ($TestTalk) { $env:COOPNET_TALKTEST = '1' } # the host talks to a citizen, the client logs what it sees
if ($TestStory) { $env:COOPNET_STORYTEST = '1' } # the host kills a stock character, the client checks it
if ($TestGive) { $env:COOPNET_GIVETEST = '1' } # the client gives the host money and potions
if ($TestSleep) { $env:COOPNET_SLEEPTEST = '1'; $env:COOPNET_AUTOVOTE = '1' } # the client sleeps, the host agrees
$env:COOPNET_TEST = 'host'
if ($TestSkip) { $env:COOPNET_TESTSKIP = '1' }
$hostProc = Start-Process (Join-Path $game 'Maelstrom.exe') -WorkingDirectory $game -PassThru
Start-Sleep 8
$env:COOPNET_TEST = 'client'
Remove-Item Env:COOPNET_TESTSKIP -ErrorAction SilentlyContinue
if ($TestSleep) { Remove-Item Env:COOPNET_AUTOVOTE -ErrorAction SilentlyContinue }
if ($TestSkip) { $env:COOPNET_AUTOVOTE = '1' }
if ($TestHero) { $env:COOPNET_HEROTEST = '1' }
$clientProc = Start-Process $dev2 -WorkingDirectory $game -PassThru
Remove-Item Env:COOPNET_TEST, Env:COOPNET_NEWGAME, Env:COOPNET_AUTOVOTE, Env:COOPNET_TOMAP, Env:COOPNET_TESTWORLD, Env:COOPNET_TOSEA, Env:COOPNET_SEAJOIN, Env:COOPNET_SEAFLAGS, Env:COOPNET_HEROTEST, Env:COOPNET_RULESTEST, Env:COOPNET_LANDTEST, Env:COOPNET_GIVETEST, Env:COOPNET_SLEEPTEST, Env:COOPNET_STORYTEST, Env:COOPNET_TALKTEST, Env:COOPNET_SWINGTEST -ErrorAction SilentlyContinue

Start-Sleep 5 # the client reads start.ini at its start
[IO.File]::WriteAllText($startIni, $startText)
Write-Host "host pid $($hostProc.Id), client pid $($clientProc.Id); waiting $Seconds s (switch between the windows)"
if ($TestHero) {
    # the client leaves and comes back as a returning player (no automatic New Game)
    Start-Sleep $Seconds
    $clientProc | Stop-Process -Force
    Start-Sleep 3
    $env:COOPNET_TEST = 'client'; $env:COOPNET_HEROBACK = '1'
    $clientProc = Start-Process $dev2 -WorkingDirectory $game -PassThru
    Remove-Item Env:COOPNET_TEST, Env:COOPNET_HEROBACK -ErrorAction SilentlyContinue
    Write-Host "client restarted, pid $($clientProc.Id)"
}
Start-Sleep $Seconds
Get-Content (Join-Path $game 'coopnet.log')
if (Test-Path (Join-Path $game 'error.log')) {
    Write-Host '--- error.log (unique lines)'
    Get-Content (Join-Path $game 'error.log') -Encoding Default | Sort-Object -Unique
}
if (-not $Keep) { $hostProc, $clientProc | Stop-Process -Force -ErrorAction SilentlyContinue }
