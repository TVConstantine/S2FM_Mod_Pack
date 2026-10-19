# S2FM Mod Pack installer. Called by Install.bat and Uninstall.bat; can also be run directly:
#   powershell -NoProfile -ExecutionPolicy Bypass -File s2fm_mods.ps1 -Action install|uninstall|find [-Game "X:\...\Half-Life Alyx"] [-Purge]
#
# install:   copies the files into the game. A file it replaces that is not ours (for example another dxgi.dll) is
#            moved to game\bin\win64\s2fm_mods_backup\ first. The ReShade panel is added to game\bin\sdkenginetools.txt.
# uninstall: removes our files, puts back what was moved, removes our entry from sdkenginetools.txt. The effects,
#            presets and settings in game\bin\win64\s2fm_reshade\ stay unless -Purge is given.
# find:      only prints the game folder that would be used.
#
# The game is found through Steam (all library folders, any drive), or given with -Game.
param(
    [ValidateSet("install", "uninstall", "find")] [string] $Action = "install",
    [string] $Game = "",
    [switch] $Purge,
    [string] $SteamDir = ""   # for tests: a Steam folder to read instead of the one in the registry
)
$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = Split-Path -Parent $Here
$AppId = "546560"

# Where each file goes (under the game folder) and where it is found: beside this script (the package) or in the
# repository's build folders.
$Files = @(
    @{ Dst = "game\bin\win64\dxgi.dll"; Src = @("dxgi.dll", "bin\core\dxgi.dll") },
    @{ Dst = "game\bin\win64\tools\s2fm_reshade_core.dll"; Src = @("tools\s2fm_reshade_core.dll", "reshade\bin\s2fm_reshade_core.dll") },
    @{ Dst = "game\bin\win64\tools\s2fm_reshade.dll"; Src = @("tools\s2fm_reshade.dll", "reshade\bin\s2fm_reshade.dll") },
    @{ Dst = "game\core\tools\images\s2fm_reshade\appicon.ico"; Src = @("images\appicon.ico", "reshade\tool\images\appicon.ico") },
    @{ Dst = "game\core\tools\images\s2fm_reshade\appicon.png"; Src = @("images\appicon.png", "reshade\tool\images\appicon.png") }
)
# Made by the mods while the game runs; removed on uninstall.
$Made = @("game\bin\win64\s2fm_shader_table.bin", "game\bin\win64\s2fm_shader_table.bin.part",
          "game\bin\win64\s2fm_mods.log", "game\bin\win64\tools\s2fm_reshade_tool.log")
# Kept on uninstall unless -Purge.
$UserData = @("game\bin\win64\s2fm_reshade", "game\bin\win64\s2fm_rtao.ini", "game\bin\win64\s2fm_mods_crash")
$Backup = "game\bin\win64\s2fm_mods_backup"
$ManifestPath = "$Backup\installed.txt"
$ToolsList = "game\bin\sdkenginetools.txt"
$Entry = "`t`t{`n`t`t`tm_Name = `"s2fm_reshade`"`n`t`t`tm_FriendlyName = `"ReShade`"`n" +
         "`t`t`tm_Library = `"tools/s2fm_reshade.dll`"`n`t`t`tm_ToolIcon = `"toolimages:s2fm_reshade/appicon.ico`"`n" +
         "`t`t`tm_PrimaryAssetTypes = [  ]`n`t`t},`n"
$Utf8 = New-Object System.Text.UTF8Encoding($false)

function Say($text) { Write-Host "[S2FM Mod Pack] $text" }

function Find-Game {
    $libraries = @()
    if ($SteamDir) { $libraries += $SteamDir }
    foreach ($key in @("HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam", "HKLM:\SOFTWARE\Valve\Steam")) {
        try {
            $item = Get-ItemProperty -Path $key -ErrorAction Stop
            if ($SteamDir) { break }
            foreach ($name in @("SteamPath", "InstallPath")) {
                if ($item.$name) { $libraries += ($item.$name -replace "/", "\") }
            }
        } catch { }
    }
    foreach ($steam in @($libraries)) {
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        if (Test-Path -LiteralPath $vdf) {
            $text = [System.IO.File]::ReadAllText($vdf, $Utf8)
            foreach ($m in [regex]::Matches($text, '"path"\s+"([^"]+)"')) {
                $libraries += ($m.Groups[1].Value -replace "\\\\", "\")
            }
        }
    }
    foreach ($lib in ($libraries | Select-Object -Unique)) {
        $acf = Join-Path $lib "steamapps\appmanifest_$AppId.acf"
        $dir = Join-Path $lib "steamapps\common\Half-Life Alyx"
        if ((Test-Path -LiteralPath $acf) -and (Test-Path -LiteralPath (Join-Path $dir "game\bin\win64"))) {
            return (Resolve-Path -LiteralPath $dir).Path
        }
    }
    return $null
}

function Source-Of($candidates) {
    foreach ($rel in $candidates) {
        foreach ($base in @($Here, $Repo)) {
            $p = Join-Path $base $rel
            if (Test-Path -LiteralPath $p) { return $p }
        }
    }
    return $null
}

function Same-File($a, $b) {
    if ((Get-Item -LiteralPath $a).Length -ne (Get-Item -LiteralPath $b).Length) { return $false }
    return (Get-FileHash -LiteralPath $a).Hash -eq (Get-FileHash -LiteralPath $b).Hash
}

function Read-Manifest($game) {
    $out = [ordered]@{}
    $p = Join-Path $game $ManifestPath
    if (Test-Path -LiteralPath $p) {
        foreach ($line in [System.IO.File]::ReadAllLines($p, $Utf8)) {
            $parts = $line -split "`t"
            if ($parts.Count -eq 2) { $out[$parts[1]] = $parts[0] }
        }
    }
    return $out
}

function Write-Manifest($game, $entries) {
    $p = Join-Path $game $ManifestPath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $p) | Out-Null
    $lines = foreach ($k in $entries.Keys) { "$($entries[$k])`t$k" }
    [System.IO.File]::WriteAllLines($p, [string[]]$lines, $Utf8)
}

# The entry's span in the tools list: from the line of its "{" to the line after its "},". $null when not there.
function Entry-Span($t) {
    $name = $t.IndexOf('m_Name = "s2fm_reshade"')
    if ($name -lt 0) { return $null }
    $open = $t.LastIndexOf("{", $name)
    $begin = $t.LastIndexOf("`n", $open) + 1
    $end = $t.IndexOf("},", $name)
    if ($open -lt 0 -or $end -lt 0) { return @(-1, -1) }
    $end = $t.IndexOf("`n", $end) + 1
    return @($begin, $end)
}

function Edit-ToolsList($game, [bool] $add) {
    $path = Join-Path $game $ToolsList
    if (-not (Test-Path -LiteralPath $path)) { return "not there ($path)" }
    $raw = [System.IO.File]::ReadAllText($path, $Utf8)
    $crlf = $raw.Contains("`r`n")
    $t = $raw.Replace("`r`n", "`n")
    $span = Entry-Span $t
    if ($add) {
        if ($null -ne $span) {
            if ($span[0] -lt 0) { return "its entry cannot be found exactly; left as it is" }
            if ($t.Substring($span[0], $span[1] - $span[0]) -eq $Entry) { return "already listed" }
            $new = $t.Substring(0, $span[0]) + $Entry + $t.Substring($span[1]); $what = "entry updated"
        } else {
            $start = $t.IndexOf("m_EngineTools")
            if ($start -lt 0) { return "it does not look like the tools list; left as it is" }
            $ext = $t.IndexOf("m_ExternalTools", $start)
            $limit = if ($ext -ge 0) { $ext } else { $t.Length }
            $close = $t.LastIndexOf("]", $limit - 1)
            if ($close -lt $start) { return "it does not look like the tools list; left as it is" }
            $lineStart = $t.LastIndexOf("`n", $close) + 1
            $new = $t.Substring(0, $lineStart) + $Entry + $t.Substring($lineStart); $what = "listed"
        }
    } else {
        if ($null -eq $span) { return "not listed" }
        if ($span[0] -lt 0) { return "its entry cannot be found exactly; left as it is" }
        $new = $t.Substring(0, $span[0]) + $t.Substring($span[1]); $what = "entry removed"
    }
    if ($crlf) { $new = $new.Replace("`n", "`r`n") }
    $tmp = "$path.s2fm_tmp"
    [System.IO.File]::WriteAllText($tmp, $new, $Utf8)
    Move-Item -LiteralPath $tmp -Destination $path -Force
    return $what
}

function Install($game) {
    $missing = @($Files | Where-Object { -not (Source-Of $_.Src) } | ForEach-Object { $_.Dst })
    if ($missing.Count) { Say "files missing from the package: $($missing -join ', ')"; return 1 }
    $entries = Read-Manifest $game
    $backupDir = Join-Path $game $Backup
    foreach ($f in $Files) {
        $src = Source-Of $f.Src
        $dst = Join-Path $game $f.Dst
        if ((Test-Path -LiteralPath $dst) -and -not $entries.Contains($f.Dst)) {
            if (Same-File $src $dst) { $entries[$f.Dst] = "new"; continue }
            $keep = Join-Path $backupDir $f.Dst
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $keep) | Out-Null
            Move-Item -LiteralPath $dst -Destination $keep -Force
            $entries[$f.Dst] = "kept"
            Say "moved to the backup: $($f.Dst)"
        } elseif (-not $entries.Contains($f.Dst)) {
            $entries[$f.Dst] = "new"
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
        Say "copied: $($f.Dst)"
    }
    $toolsPath = Join-Path $game $ToolsList
    $toolsKept = Join-Path $backupDir "sdkenginetools.txt"
    if ((Test-Path -LiteralPath $toolsPath) -and -not (Test-Path -LiteralPath $toolsKept)) {
        New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
        Copy-Item -LiteralPath $toolsPath -Destination $toolsKept
    }
    $what = Edit-ToolsList $game $true
    Say "ReShade panel in sdkenginetools.txt: $what"
    if ($what -eq "listed") { $entries[$ToolsList] = "entry" }
    Write-Manifest $game $entries
    Say "installed into $game"
    return 0
}

function Uninstall($game) {
    $entries = Read-Manifest $game
    if ($entries.Count -eq 0) { Say "nothing installed here ($ManifestPath not found)"; return 1 }
    $backupDir = Join-Path $game $Backup
    foreach ($rel in $entries.Keys) {
        if ($entries[$rel] -eq "entry") { continue }
        $dst = Join-Path $game $rel
        if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Force }
        if ($entries[$rel] -eq "kept") {
            $keep = Join-Path $backupDir $rel
            if (Test-Path -LiteralPath $keep) { Move-Item -LiteralPath $keep -Destination $dst -Force; Say "put back: $rel" }
            else { Say "missing from the backup: $rel" }
        } else {
            Say "removed: $rel"
        }
    }
    foreach ($rel in $Made) {
        $p = Join-Path $game $rel
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
    }
    $icons = Join-Path $game "game\core\tools\images\s2fm_reshade"
    if ((Test-Path -LiteralPath $icons) -and -not (Get-ChildItem -LiteralPath $icons)) { Remove-Item -LiteralPath $icons }
    if ($entries.Contains($ToolsList)) { Say "ReShade panel in sdkenginetools.txt: $(Edit-ToolsList $game $false)" }
    else { Say "ReShade panel in sdkenginetools.txt: it was there before the install, left in" }
    if ($Purge) {
        foreach ($rel in $UserData) {
            $p = Join-Path $game $rel
            if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force; Say "removed: $rel" }
        }
    }
    Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction SilentlyContinue
    Say "uninstalled from $game"
    return 0
}

try {
    if (-not $Game) { $Game = Find-Game }
    if (-not $Game) {
        Say "Half-Life: Alyx was not found through Steam."
        $Game = Read-Host "Paste the Half-Life Alyx folder (the one that has the 'game' folder in it)"
        $Game = $Game.Trim('"', ' ')
    }
    if (-not $Game -or -not (Test-Path -LiteralPath (Join-Path $Game "game\bin\win64"))) {
        Say "not a Half-Life: Alyx folder: $Game"
        exit 1
    }
    Say "game folder: $Game"
    if ($Action -eq "find") { exit 0 }
    if (Get-Process -Name hlvr -ErrorAction SilentlyContinue) { Say "the game is running: close it first"; exit 1 }
    if ($Action -eq "install") { exit (Install $Game) } else { exit (Uninstall $Game) }
} catch [System.UnauthorizedAccessException] {
    Say "no permission to write into the game folder. Run the .bat file as administrator."
    exit 1
} catch {
    Say "failed: $($_.Exception.Message)"
    exit 1
}
