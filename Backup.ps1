#Requires -Version 5.1
<#
.SYNOPSIS
    Hamster Backup engine - copies new and changed files of selected folders to the NAS.

.DESCRIPTION
    Uses robocopy (built into Windows). Files on the NAS are plain files in the
    same folder structure as on the laptop, so you can open them directly.

    Rules:
      * Only new and changed files are copied.
      * Before a changed file is overwritten on the NAS, the old version is moved
        to .versionen\<date_time>\...
      * Files you delete or move on the laptop are NEVER deleted on the NAS.
      * Files that exist only on the NAS are listed (state\nas-only.json). You decide
        per item in the Hamster app (or with -Review); "keep" is stored in .behalten.txt.

    The engine reports to state\status.json and state\history.csv, which the
    Hamster tray app reads. Only the process holding the engine mutex writes them.

.EXAMPLE
    .\Backup.ps1              # run the backup now (console, with progress)
    .\Backup.ps1 -DryRun      # show what would happen, change nothing
    .\Backup.ps1 -Report      # list files/folders that exist only on the NAS
    .\Backup.ps1 -Review      # decide about those items in the console
    .\Backup.ps1 -Scheduled   # automatic run: once per day after DailyAfter, quiet
    .\Backup.ps1 -Scheduled -Force   # quiet, but run now (used by "Backup now" in the app)
#>
[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Report,
    [switch]$Review,
    [switch]$Scheduled,
    [switch]$Force,
    [string]$ConfigPath
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# $PSScriptRoot is not yet set inside param() in Windows PowerShell 5.1 when started via -File
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $ConfigPath) { $ConfigPath = Join-Path $ScriptDir 'backup-config.psd1' }

$EngineVersion   = '2026-11-22'
$EngineMutexName = 'Local\HamsterBackup.Engine'
$AppMutexName    = 'Local\HamsterBackup.App'
$VersionsDirName = '.versionen'
$LogsDirName     = '.logs'
$KeepListName    = '.behalten.txt'

if (-not (Test-Path -LiteralPath $ConfigPath)) {
    Write-Host "Config not found: $ConfigPath`nCopy backup-config.example.psd1 to backup-config.psd1 and adjust it." -ForegroundColor Red
    exit 2
}
$cfg         = Import-PowerShellDataFile -LiteralPath $ConfigPath
$TargetRoot  = ([string]$cfg.TargetRoot).TrimEnd('\')
$SourceRoot  = ([string]$cfg.SourceRoot).TrimEnd('\')
$Stamp       = Get-Date -Format 'yyyy-MM-dd_HHmmss'
$StateDir    = Join-Path $ScriptDir 'state'
$LocalLogDir = Join-Path $ScriptDir 'logs'
$StatusFile  = Join-Path $StateDir 'status.json'
$HistoryFile = Join-Path $StateDir 'history.csv'
$NasOnlyFile = Join-Path $StateDir 'nas-only.json'
$PauseFile   = Join-Path $StateDir 'paused-until.txt'
$StopFile    = Join-Path $StateDir 'stop.request'

$DailyAfter = '16:00'
if ($cfg.ContainsKey('DailyAfter') -and $cfg.DailyAfter) { $DailyAfter = [string]$cfg.DailyAfter }

if ($DryRun)      { $Mode = 'dryrun' }
elseif ($Report)  { $Mode = 'report' }
elseif ($Review)  { $Mode = 'review' }
else              { $Mode = 'backup' }
if ($Force -or -not $Scheduled) { $Trigger = 'manual' } else { $Trigger = 'scheduled' }

# robocopy exclusions; the engine's own state/logs folders are excluded by full path
# (they live inside Projekte, which is backed up, and change during the run)
$ExcludeArgs = @()
if ($cfg.ExcludeFiles) { $ExcludeArgs += '/XF'; $ExcludeArgs += @($cfg.ExcludeFiles) }
$ExcludeArgs += '/XD'
if ($cfg.ExcludeDirs) { $ExcludeArgs += @($cfg.ExcludeDirs) }
$ExcludeArgs += @($StateDir, $LocalLogDir)

$script:LogFile  = $null
$script:Warnings = 0
$script:Errors   = 0
$script:Stopped  = $false

# ---------------------------------------------------------------- helpers

function Write-Log {
    param([string]$Message, [ValidateSet('INFO', 'WARN', 'ERROR', 'OK')][string]$Level = 'INFO')
    $line = '{0} [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Level, $Message
    if ($Level -eq 'WARN')  { $script:Warnings++ }
    if ($Level -eq 'ERROR') { $script:Errors++ }
    if (-not $Scheduled) {
        $color = @{ INFO = 'Gray'; WARN = 'Yellow'; ERROR = 'Red'; OK = 'Green' }[$Level]
        Write-Host $line -ForegroundColor $color
    }
    if ($script:LogFile) {
        try { Add-Content -LiteralPath $script:LogFile -Value $line -Encoding UTF8 } catch { }
    }
}

function Get-NowMs { return [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }

function ConvertTo-LongPath([string]$Path) {
    # \\?\ prefix lets .NET handle paths longer than 260 characters
    if ($Path.StartsWith('\\?\')) { return $Path }
    if ($Path.StartsWith('\\'))   { return '\\?\UNC\' + $Path.Substring(2) }
    return '\\?\' + $Path
}

function Test-DirectoryExists([string]$Path) {
    return [IO.Directory]::Exists((ConvertTo-LongPath $Path))
}

function New-Directory([string]$Path) {
    [void][IO.Directory]::CreateDirectory((ConvertTo-LongPath $Path))
}

function Move-Entry([string]$From, [string]$To) {
    # Moves a file or folder within the NAS share (a rename on the NAS, no copying)
    New-Directory (Split-Path -Path $To -Parent)
    $longFrom = ConvertTo-LongPath $From
    $longTo   = ConvertTo-LongPath $To
    if ([IO.Directory]::Exists($longFrom)) { [IO.Directory]::Move($longFrom, $longTo) }
    else { [IO.File]::Move($longFrom, $longTo) }
}

function Format-Size([int64]$Bytes) {
    if ($Bytes -ge 1TB) { return '{0:N2} TB' -f ($Bytes / 1TB) }
    if ($Bytes -ge 1GB) { return '{0:N2} GB' -f ($Bytes / 1GB) }
    if ($Bytes -ge 1MB) { return '{0:N1} MB' -f ($Bytes / 1MB) }
    if ($Bytes -ge 1KB) { return '{0:N0} KB' -f ($Bytes / 1KB) }
    return "$Bytes B"
}

function Get-State([string]$Name) {
    $p = Join-Path $StateDir "$Name.txt"
    try { if (Test-Path -LiteralPath $p) { return (Get-Content -LiteralPath $p -Raw).Trim() } } catch { }
    return $null
}

function Set-State([string]$Name, [string]$Value) {
    try {
        New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
        Set-Content -LiteralPath (Join-Path $StateDir "$Name.txt") -Value $Value -Encoding UTF8
    } catch { }
}

function Test-StateIsToday([string]$Name) {
    $v = Get-State $Name
    if (-not $v) { return $false }
    try { return ([datetime]::Parse($v)).Date -eq (Get-Date).Date } catch { return $false }
}

function Test-AppRunning {
    $m = $null
    try {
        if ([Threading.Mutex]::TryOpenExisting($AppMutexName, [ref]$m)) { $m.Dispose(); return $true }
    } catch { }
    return $false
}

function Show-Toast([string]$Title, [string]$Text) {
    # The Hamster app shows its own notifications; only notify if it isn't running
    if (Test-AppRunning) { return }
    try {
        $null = [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime]
        $null = [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime]
        $t = [Security.SecurityElement]::Escape($Title)
        $b = [Security.SecurityElement]::Escape($Text)
        $xml = New-Object Windows.Data.Xml.Dom.XmlDocument
        $xml.LoadXml("<toast><visual><binding template='ToastGeneric'><text>$t</text><text>$b</text></binding></visual></toast>")
        $appId = '{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe'
        $toast = New-Object Windows.UI.Notifications.ToastNotification $xml
        [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($appId).Show($toast)
    }
    catch { }
}

function Show-ToastOncePerDay([string]$Key, [string]$Title, [string]$Text) {
    # Scheduled runs repeat every 30 min; don't show the same warning over and over
    if ($Scheduled -and (Test-StateIsToday $Key)) { return }
    Show-Toast $Title $Text
    Set-State $Key (Get-Date -Format o)
}

# ---------------------------------------------------------------- keep awake

$script:PowerApi = $null
function Set-KeepAwake([bool]$On) {
    try {
        if (-not $script:PowerApi) {
            $script:PowerApi = Add-Type -PassThru -Name 'HamsterPower' -Namespace 'HamsterBackup' -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint esFlags);
'@
        }
        # ES_CONTINUOUS | ES_SYSTEM_REQUIRED keeps the PC from idle sleep (lid close still sleeps)
        if ($On) { [void]$script:PowerApi::SetThreadExecutionState([uint32]'0x80000001') }
        else     { [void]$script:PowerApi::SetThreadExecutionState([uint32]'0x80000000') }
    } catch { }
}

# ---------------------------------------------------------------- status / history (read by the app)

function Get-JsonProp($Obj, [string]$Name) {
    if ($null -eq $Obj) { return $null }
    $p = $Obj.PSObject.Properties[$Name]
    if ($p) { return $p.Value }
    return $null
}

function Write-FileAtomic([string]$Path, [string]$Text) {
    # Write to a temp file in the same folder, then replace. Never throws.
    $tmp = $Path + '.tmp'
    for ($i = 0; $i -lt 4; $i++) {
        try {
            New-Item -ItemType Directory -Force -Path (Split-Path $Path -Parent) | Out-Null
            [IO.File]::WriteAllText($tmp, $Text, (New-Object Text.UTF8Encoding $false))
            if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($tmp, $Path, [NullString]::Value) }
            else { [IO.File]::Move($tmp, $Path) }
            return
        }
        catch { Start-Sleep -Milliseconds 80 }
    }
}

$script:St = [ordered]@{
    schema         = 1
    writtenUtc     = 0
    engine         = $null
    config         = $null
    lastCheck      = $null
    current        = $null
    lastRun        = $null
    lastSuccessUtc = $null
    nasOnly        = $null
}

function Initialize-Status {
    # Load the previous status (keeps lastRun / lastSuccess / nasOnly); returns the old "current"
    $old = $null
    try {
        if (Test-Path -LiteralPath $StatusFile) {
            $old = [IO.File]::ReadAllText($StatusFile) | ConvertFrom-Json
        }
    } catch { $old = $null }
    if ($old) {
        $script:St.lastRun        = Get-JsonProp $old 'lastRun'
        $script:St.lastSuccessUtc = Get-JsonProp $old 'lastSuccessUtc'
        $script:St.nasOnly        = Get-JsonProp $old 'nasOnly'
        $script:St.lastCheck      = Get-JsonProp $old 'lastCheck'
    }
    if (-not $script:St.lastSuccessUtc) {
        # migrate from the older state\last-success.txt
        $ls = Get-State 'last-success'
        if ($ls) { try { $script:St.lastSuccessUtc = ([DateTimeOffset][datetime]::Parse($ls)).ToUnixTimeMilliseconds() } catch { } }
    }
    $proc = Get-Process -Id $PID
    $script:St.engine = [ordered]@{
        version     = $EngineVersion
        pid         = $PID
        pidStartUtc = ([DateTimeOffset]$proc.StartTime).ToUnixTimeMilliseconds()
    }
    $script:St.config = [ordered]@{
        targetRoot    = $TargetRoot
        sourceRoot    = $SourceRoot
        dailyAfter    = $DailyAfter
        warnAfterDays = [int]$cfg.WarnAfterDays
        folderCount   = @($cfg.Folders).Count
    }
    return (Get-JsonProp $old 'current')
}

function Save-Status {
    try {
        $script:St.writtenUtc = Get-NowMs
        $json = $script:St | ConvertTo-Json -Depth 6 -Compress
        Write-FileAtomic $StatusFile $json
    } catch { }
}

function Set-LastCheck([string]$Result) {
    $script:St.lastCheck = [ordered]@{ utc = (Get-NowMs); result = $Result }
    Save-Status
}

function Add-History($Run) {
    try {
        New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
        $msg = ([string]$Run.message) -replace '[;\r\n]', ','
        $line = '{0};{1};{2};{3};{4};{5};{6};{7};{8};{9};{10};{11};{12}' -f `
            $Run.runId, $Run.startedUtc, $Run.endedUtc, $Run.mode, $Run.trigger, $Run.result, $Run.exitCode,
            $Run.filesCopied, $Run.bytesCopied, $Run.versioned, $Run.warnings, $Run.errors, $msg
        $prefix = ''
        if (-not (Test-Path -LiteralPath $HistoryFile)) {
            $prefix = [char]0xFEFF + 'runId;startedUtc;endedUtc;mode;trigger;result;exitCode;filesCopied;bytesCopied;versioned;warnings;errors;message' + "`r`n"
        }
        else {
            # a crash mid-write could leave a partial line: start on a fresh line
            $fs = [IO.File]::Open($HistoryFile, 'Open', 'Read', 'ReadWrite')
            try {
                if ($fs.Length -gt 0) { [void]$fs.Seek(-1, 'End'); if ($fs.ReadByte() -ne 10) { $prefix = "`r`n" } }
            } finally { $fs.Dispose() }
        }
        [IO.File]::AppendAllText($HistoryFile, $prefix + $line + "`r`n", (New-Object Text.UTF8Encoding $false))
    } catch { }
}

function Complete-Run([string]$Result, [int]$ExitCode, [string]$Message) {
    $c = $script:St.current
    if (-not $c) { return }
    $run = [ordered]@{
        runId       = $c.runId
        mode        = $c.mode
        trigger     = $c.trigger
        startedUtc  = $c.startedUtc
        endedUtc    = Get-NowMs
        result      = $Result
        exitCode    = $ExitCode
        filesCopied = $c.filesDone
        bytesCopied = $c.bytesDone
        versioned   = $c.versioned
        warnings    = $script:Warnings
        errors      = $script:Errors
        message     = $Message
        logFile     = $c.logFile
    }
    if ($c.mode -eq 'backup') {
        $script:St.lastRun = $run
        if ($Result -eq 'ok' -or $Result -eq 'warnings') { $script:St.lastSuccessUtc = $run.endedUtc }
    }
    $script:St.current = $null
    Add-History $run
    Save-Status
}

# ---------------------------------------------------------------- robocopy

function Join-ProcessArguments([string[]]$Items) {
    # Quote for the Windows command line; our paths never end with a backslash
    $parts = foreach ($a in $Items) {
        if ($a -eq '' -or $a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a }
    }
    return ($parts -join ' ')
}

function Get-RobocopyFileList {
    # Runs robocopy in list-only mode (/L) and returns the files it would copy
    # as objects with Rel (path relative to $From) and Size.
    param([string]$From, [string]$To, [string[]]$Options)
    $tmp = Join-Path $env:TEMP ('hamster_{0}.log' -f [guid]::NewGuid())
    try {
        $rcArgs = @($From, $To, '/L', '/E', '/XJ', '/XX', '/FFT', '/R:0', '/W:0',
                    '/NJH', '/NJS', '/NDL', '/NC', '/BYTES', '/FP', "/UNILOG:$tmp") + $Options + $ExcludeArgs
        $null = & robocopy.exe @rcArgs
        $code = $LASTEXITCODE
        if ($code -ge 8) { throw "robocopy list failed (exit $code): $From -> $To" }
        $result = New-Object System.Collections.Generic.List[object]
        if (-not (Test-Path -LiteralPath $tmp)) { return $result }
        $prefix = $From.TrimEnd('\') + '\'
        foreach ($line in [IO.File]::ReadAllLines($tmp, [Text.Encoding]::Unicode)) {
            if ($line -match '^\s*(\d+)\s+(.+?)\s*$') {
                $size = [int64]$Matches[1]
                $path = $Matches[2]
                if ($path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                    $result.Add([pscustomobject]@{ Rel = $path.Substring($prefix.Length); Size = $size })
                }
            }
        }
        return $result
    }
    finally {
        Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
    }
}

function Invoke-RobocopyTracked([string[]]$RcArgs, [string]$UniLog) {
    # Runs robocopy as a child process. Every ~3 s: reads newly finished files from
    # its log (progress), writes status.json, and checks for a stop request.
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName        = Join-Path $env:SystemRoot 'System32\robocopy.exe'
    $psi.Arguments       = Join-ProcessArguments ($RcArgs + @("/UNILOG:$UniLog"))
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow  = $true
    $p = [Diagnostics.Process]::Start($psi)

    $c = $script:St.current
    $pos = [int64]0
    $rest = ''
    $lastBytes = [int64]$c.bytesDone
    $lastTime = Get-NowMs
    # robocopy logs "<class> <size> <path>" when a file starts, then "  42.5%" lines while it copies
    $fileRx = New-Object Text.RegularExpressions.Regex '\s(\d+)\s+((?:[A-Za-z]:\\|\\\\)[^\t]+?)\s*$'
    $pctRx  = New-Object Text.RegularExpressions.Regex '^\s*(\d{1,3}(?:[.,]\d+)?)%\s*$'
    $baseBytes = [int64]$c.bytesDone      # bytes of files finished before the current one
    $curSize = [int64]-1                  # size of the file being copied (-1 = none)
    $done = $false
    while (-not $done) {
        $done = $p.WaitForExit(3000)
        # read what robocopy has logged since last time (UTF-16)
        try {
            if (Test-Path -LiteralPath $UniLog) {
                $fs = [IO.File]::Open($UniLog, 'Open', 'Read', 'ReadWrite, Delete')
                try {
                    if ($fs.Length -gt $pos) {
                        [void]$fs.Seek($pos, 'Begin')
                        $count = [int][math]::Min($fs.Length - $pos, 16MB)
                        $count -= ($count % 2)
                        $buf = New-Object byte[] $count
                        $read = $fs.Read($buf, 0, $count)
                        $read -= ($read % 2)
                        $pos += $read
                        $text = $rest + [Text.Encoding]::Unicode.GetString($buf, 0, $read)
                        $lines = $text -split "[`r`n]+"     # progress updates are separated by CR only
                        $rest = $lines[-1]
                        for ($i = 0; $i -lt $lines.Length - 1; $i++) {
                            $ln = $lines[$i]
                            $m = $pctRx.Match($ln)
                            if ($m.Success) {
                                if ($curSize -ge 0) {
                                    $pct = [double]::Parse($m.Groups[1].Value.Replace(',', '.'), [Globalization.CultureInfo]::InvariantCulture)
                                    $c.bytesDone = $baseBytes + [int64]($curSize * [math]::Min(100.0, $pct) / 100.0)
                                }
                                continue
                            }
                            $m = $fileRx.Match($ln)
                            if ($m.Success) {
                                if ($curSize -ge 0) { $baseBytes += $curSize; $c.filesDone++ }   # previous file finished
                                $curSize = [int64]$m.Groups[1].Value
                                $c.bytesDone = $baseBytes
                                $c.currentFile = $m.Groups[2].Value
                            }
                        }
                    }
                } finally { $fs.Dispose() }
            }
        } catch { }

        $now = Get-NowMs
        $dt = $now - $lastTime
        if ($dt -ge 2500) {
            $speed = [double]($c.bytesDone - $lastBytes) * 1000.0 / $dt
            # smooth the speed a little
            if ($c.bytesPerSec -gt 0) { $speed = 0.6 * $speed + 0.4 * [double]$c.bytesPerSec }
            $c.bytesPerSec = [int64][math]::Max(0, $speed)
            $lastBytes = $c.bytesDone
            $lastTime = $now
        }
        $c.heartbeatUtc = $now
        Save-Status

        if (-not $Scheduled) {
            $pct = 0
            if ($c.bytesPlanned -gt 0) { $pct = [math]::Min(100, [math]::Floor(100.0 * $c.bytesDone / $c.bytesPlanned)) }
            Write-Host ("  {0}  {1}/{2} files  {3} of {4}  {5}/s" -f "$pct %".PadLeft(5), $c.filesDone, $c.filesPlanned,
                        (Format-Size $c.bytesDone), (Format-Size $c.bytesPlanned), (Format-Size $c.bytesPerSec)) -ForegroundColor DarkGray
        }

        if (-not $done -and (Test-Path -LiteralPath $StopFile)) {
            $script:Stopped = $true
            try { $p.Kill() } catch { }
            [void]$p.WaitForExit(15000)
            $done = $true
        }
    }
    $code = $p.ExitCode
    $p.Dispose()
    if ($curSize -ge 0 -and -not $script:Stopped) { $baseBytes += $curSize; $c.filesDone++ }   # last file finished
    $c.bytesDone = $baseBytes
    $c.currentFile = ''
    return $code
}

function Add-LogToNas([string]$LocalLog, [string]$NasLog) {
    # Append this folder's robocopy log to the run's log on the NAS, without the "42.5%" progress lines
    try {
        if (-not (Test-Path -LiteralPath $LocalLog)) { return }
        $rx = New-Object Text.RegularExpressions.Regex '^\s*\d{1,3}(?:[.,]\d+)?%\s*$'
        $reader = New-Object IO.StreamReader($LocalLog, [Text.Encoding]::Unicode, $true)
        try {
            $writer = New-Object IO.StreamWriter($NasLog, $true, [Text.Encoding]::Unicode)
            try {
                while ($null -ne ($line = $reader.ReadLine())) {
                    # progress updates are separated by CR only: keep the text after the last one
                    $parts = $line -split "`r"
                    foreach ($part in $parts) { if ($part -and -not $rx.IsMatch($part)) { $writer.WriteLine($part) } }
                }
            } finally { $writer.Dispose() }
        } finally { $reader.Dispose() }
    } catch { Write-Log "  could not copy the robocopy log to the NAS: $($_.Exception.Message)" 'INFO' }
    Remove-Item -LiteralPath $LocalLog -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- backup

function Invoke-Backup([string]$LogDir) {
    $versionRoot = Join-Path (Join-Path $TargetRoot $VersionsDirName) $Stamp
    $rcLog       = Join-Path $LogDir "$Stamp.robocopy.log"
    $localRcLog  = Join-Path $LocalLogDir 'current.robocopy.log'
    $c = $script:St.current
    $folders = @($cfg.Folders)

    # 1) Scan: what has to be copied (gives the progress total)
    $c.phase = 'scanning'
    Save-Status
    $plan = @()
    $i = 0
    foreach ($f in $folders) {
        $i++
        if ($script:Stopped -or (Test-Path -LiteralPath $StopFile)) { $script:Stopped = $true; return }
        $src = Join-Path $SourceRoot $f.Source
        $dst = Join-Path $TargetRoot $f.Target
        $c.folder = $f.Source; $c.folderIndex = $i
        Save-Status
        if (-not (Test-DirectoryExists $src)) {
            Write-Log "$($f.Source): not on the laptop - skipped (the copy on the NAS stays as it is)"
            continue
        }
        $files = @(Get-RobocopyFileList -From $src -To $dst -Options @())
        $bytes = [int64]0
        foreach ($x in $files) { $bytes += $x.Size }
        $c.filesPlanned += $files.Count
        $c.bytesPlanned += $bytes
        $plan += [pscustomobject]@{ Folder = $f; Src = $src; Dst = $dst; Files = $files; Index = $i }
    }
    Write-Log ("To copy: {0} file(s), {1}" -f $c.filesPlanned, (Format-Size $c.bytesPlanned))

    # 2) Per folder: keep old versions of changed files, then copy
    foreach ($item in $plan) {
        if ($script:Stopped -or (Test-Path -LiteralPath $StopFile)) { $script:Stopped = $true; return }
        $f = $item.Folder
        $c.folder = $f.Source; $c.folderIndex = $item.Index
        if ($item.Files.Count -eq 0) { Write-Log "$($f.Source) -> $($f.Target): already up to date" 'OK'; continue }
        Write-Log "$($f.Source) -> $($f.Target): $($item.Files.Count) file(s)"

        if (Test-DirectoryExists $item.Dst) {
            $c.phase = 'versioning'
            Save-Status
            foreach ($x in $item.Files) {
                $from = Join-Path $item.Dst $x.Rel
                $long = ConvertTo-LongPath $from
                if (-not [IO.File]::Exists($long)) { continue }          # new file, nothing to keep
                # a file left half-copied by an interrupted run has robocopy's 1980 date: just overwrite it
                if ([IO.File]::GetLastWriteTimeUtc($long).Year -le 1980) { continue }
                $to = Join-Path (Join-Path $versionRoot $f.Target) $x.Rel
                if ($DryRun) { Write-Log "  [dry run] would keep old version of $($f.Target)\$($x.Rel)"; continue }
                try { Move-Entry $from $to; $c.versioned++ }
                catch { Write-Log "  could not keep old version of $($f.Target)\$($x.Rel): $($_.Exception.Message)" 'WARN' }
            }
        }

        $c.phase = 'copying'
        Save-Status
        # Single-threaded by default: robocopy then reports progress live (with /MT it holds the log back)
        $rcArgs = @($item.Src, $item.Dst, '/E', '/COPY:DAT', '/DCOPY:DAT', '/FFT', '/XJ', '/XX', '/R:3', '/W:10',
                    '/NDL', '/BYTES', '/FP') + $ExcludeArgs
        if ([int]$cfg.Threads -gt 1) { $rcArgs += "/MT:$($cfg.Threads)" }
        if ($cfg.ContainsKey('ExtraRobocopyArgs') -and $cfg.ExtraRobocopyArgs) { $rcArgs += @($cfg.ExtraRobocopyArgs) }
        if ($DryRun) { $rcArgs += '/L' }
        Remove-Item -LiteralPath $localRcLog -ErrorAction SilentlyContinue
        $code = Invoke-RobocopyTracked -RcArgs $rcArgs -UniLog $localRcLog
        if ($DryRun) { $logTarget = Join-Path $LogDir "$Stamp.robocopy.log" } else { $logTarget = $rcLog }
        Add-LogToNas $localRcLog $logTarget

        if ($script:Stopped) { Write-Log "  stopped by user" 'INFO'; return }
        if ($code -ge 16)    { Write-Log "  robocopy failed (exit $code) - details in $logTarget" 'ERROR' }
        elseif ($code -ge 8) { Write-Log "  some files could not be copied (exit $code), often because they are open in another program - details in $logTarget" 'WARN' }
        elseif ($DryRun)     { Write-Log "  [dry run] files listed in $logTarget" 'OK' }
        else                 { Write-Log '  done' 'OK' }
    }
}

# ---------------------------------------------------------------- NAS-only files

function Get-KeepList {
    $p = Join-Path $TargetRoot $KeepListName
    if (-not (Test-Path -LiteralPath $p)) { return @() }
    return @(Get-Content -LiteralPath $p -Encoding UTF8 |
             ForEach-Object { $_.Trim() } |
             Where-Object { $_ -and -not $_.StartsWith('#') })
}

function Add-KeepEntry([string]$Item) {
    $p = Join-Path $TargetRoot $KeepListName
    if (-not (Test-Path -LiteralPath $p)) {
        $header = @(
            '# Hamster Backup: these files/folders exist only on the NAS and you decided to keep them.',
            '# They are never reported again. One path per line, relative to the share. A line ending',
            '# with \ covers the whole folder. You can edit this file by hand.'
        )
        Set-Content -LiteralPath $p -Value $header -Encoding UTF8
    }
    Add-Content -LiteralPath $p -Value $Item -Encoding UTF8
}

function Test-Kept([string]$Item, [string[]]$Keep) {
    $i = $Item.TrimEnd('\')
    foreach ($k in $Keep) {
        $kd = $k.TrimEnd('\')
        if ($i -ieq $kd) { return $true }
        if ($i.StartsWith($kd + '\', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

function Get-OrphanItems {
    # Files on the NAS that no longer exist on the laptop, grouped by the highest
    # folder that is missing on the laptop. Items in .behalten.txt are left out.
    $keep     = @(Get-KeepList)
    $items    = @{}
    $dirCache = @{}
    foreach ($f in $cfg.Folders) {
        $src = Join-Path $SourceRoot $f.Source
        $dst = Join-Path $TargetRoot $f.Target
        if (-not (Test-DirectoryExists $dst)) { continue }
        $srcExists = Test-DirectoryExists $src
        $files = @(Get-RobocopyFileList -From $dst -To $src -Options @('/XC', '/XN', '/XO'))
        foreach ($file in $files) {
            $rel = $null
            if (-not $srcExists) {
                $rel = ''
            }
            else {
                $parts = $file.Rel.Split('\')
                $acc = ''
                for ($i = 0; $i -lt $parts.Length - 1; $i++) {
                    if ($acc) { $acc = $acc + '\' + $parts[$i] } else { $acc = $parts[$i] }
                    $key = $src + '\' + $acc
                    if (-not $dirCache.ContainsKey($key)) { $dirCache[$key] = Test-DirectoryExists $key }
                    if (-not $dirCache[$key]) { $rel = $acc + '\'; break }
                }
                if ($null -eq $rel) { $rel = $file.Rel }
            }
            $item = $f.Target + '\' + $rel
            if (Test-Kept $item $keep) { continue }
            if (-not $items.ContainsKey($item)) {
                $local = $src
                if ($rel) { $local = $src + '\' + $rel.TrimEnd('\') }
                $kind = 'file'
                if ($item.EndsWith('\')) { $kind = 'folder' }
                $items[$item] = [pscustomobject]@{ Item = $item; Kind = $kind; Files = 0; Bytes = [int64]0; Local = $local }
            }
            $items[$item].Files++
            $items[$item].Bytes += $file.Size
        }
    }
    return @($items.Values | Sort-Object Item)
}

function Invoke-OrphanReport([string]$LogDir, [switch]$Quiet) {
    Write-Log 'Looking for files that exist only on the NAS ...'
    if ($script:St.current) { $script:St.current.phase = 'orphan_report'; Save-Status }
    $items = @(Get-OrphanItems)
    Set-State 'last-orphan-report' (Get-Date -Format o)

    $total = [int64]0
    $list = foreach ($it in $items) {
        $total += $it.Bytes
        [ordered]@{ path = $it.Item; kind = $it.Kind; files = $it.Files; bytes = $it.Bytes; localPath = $it.Local }
    }
    $now = Get-NowMs
    $doc = [ordered]@{ schema = 1; reportUtc = $now; items = @($list) }
    try { Write-FileAtomic $NasOnlyFile ($doc | ConvertTo-Json -Depth 5 -Compress) } catch { }
    $script:St.nasOnly = [ordered]@{ reportUtc = $now; count = $items.Count; bytes = $total }
    Save-Status

    if ($items.Count -eq 0) {
        Write-Log 'Nothing new: every file on the NAS exists on the laptop or is in .behalten.txt' 'OK'
        return 0
    }
    $lines = foreach ($it in $items) { '{0,12}  {1,7} file(s)  {2}' -f (Format-Size $it.Bytes), $it.Files, $it.Item }
    $reportFile = Join-Path $LogDir "$Stamp.nur-auf-NAS.txt"
    $header = @("Files/folders that exist only on the NAS ($($items.Count) item(s), $(Format-Size $total)).",
                'Open Hamster -> Review to decide. Nothing is deleted automatically.', '')
    try { Set-Content -LiteralPath $reportFile -Value ($header + $lines) -Encoding UTF8 } catch { }
    if (-not $Quiet) { $lines | ForEach-Object { Write-Host $_ } }
    Write-Log ("{0} item(s) ({1}) exist only on the NAS" -f $items.Count, (Format-Size $total)) 'INFO'
    return $items.Count
}

function Invoke-Review {
    $items = @(Get-OrphanItems)
    if ($items.Count -eq 0) {
        Write-Host 'Nothing to review: every file on the NAS exists on the laptop or is already marked to keep.' -ForegroundColor Green
        return
    }
    Write-Host ''
    Write-Host "$($items.Count) item(s) exist only on the NAS (deleted, moved or renamed on the laptop)." -ForegroundColor Cyan
    Write-Host '  [k] keep forever        - stays on the NAS, never asked again'
    Write-Host '  [v] move to old versions - moves it to .versionen (e.g. leftover after a rename)'
    Write-Host '  [l] ask later'
    Write-Host '  [q] quit'
    Write-Host ''
    $reviewRoot = Join-Path (Join-Path $TargetRoot $VersionsDirName) ($Stamp + '_review')
    $n = 0
    foreach ($it in $items) {
        $n++
        Write-Host ("[{0}/{1}] {2}  ({3}, {4}, {5} file(s))" -f $n, $items.Count, $it.Item, $it.Kind, (Format-Size $it.Bytes), $it.Files) -ForegroundColor White
        $answer = ''
        while ($answer -notin @('k', 'v', 'l', 'q')) { $answer = (Read-Host '  k/v/l/q').Trim().ToLower() }
        switch ($answer) {
            'k' { Add-KeepEntry $it.Item; Write-Log "kept: $($it.Item)" 'OK' }
            'v' {
                $from = Join-Path $TargetRoot $it.Item.TrimEnd('\')
                $to   = Join-Path $reviewRoot $it.Item.TrimEnd('\')
                try { Move-Entry $from $to; Write-Log "moved to ${VersionsDirName}: $($it.Item)" 'OK' }
                catch { Write-Log "could not move $($it.Item): $($_.Exception.Message)" 'WARN' }
            }
            'l' { }
            'q' { return }
        }
    }
}

# ---------------------------------------------------------------- schedule checks

function Test-Paused {
    # state\paused-until.txt: "forever" or a date/time (written by the Hamster app)
    if (-not (Test-Path -LiteralPath $PauseFile)) { return $false }
    $v = ''
    try { $v = (Get-Content -LiteralPath $PauseFile -Raw).Trim() } catch { return $false }
    if ($v -ieq 'forever') { return $true }
    try {
        if ([datetime]::Parse($v) -gt (Get-Date)) { return $true }
        Remove-Item -LiteralPath $PauseFile -ErrorAction SilentlyContinue    # pause is over
    } catch { }
    return $false
}

function Test-ScheduledRunDue {
    # Automatic runs: once per day, only after DailyAfter (e.g. 16:00)
    $notBefore = (Get-Date).Date + [timespan]::Parse($DailyAfter)
    if ((Get-Date) -lt $notBefore) { return 'not_due' }
    if (Test-StateIsToday 'last-success') { return 'done_today' }
    return 'due'
}

function Test-BackupAge {
    $last = Get-State 'last-success'
    if (-not $last) { return }
    $days = ((Get-Date) - [datetime]::Parse($last)).TotalDays
    if ($days -ge $cfg.WarnAfterDays) {
        $msg = 'No successful backup for {0:N0} days. Is the NAS reachable?' -f [math]::Floor($days)
        Write-Log $msg 'WARN'
        Show-ToastOncePerDay 'last-age-warning' 'Hamster Backup' $msg
    }
}

function Remove-OldLogs([string]$LogDir) {
    $limit = (Get-Date).AddDays(-[int]$cfg.KeepLogsDays)
    Get-ChildItem -LiteralPath $LogDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -lt $limit } |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- main

$exitCode = 0
$mutex = New-Object System.Threading.Mutex($false, $EngineMutexName)
$haveMutex = $false
try { $haveMutex = $mutex.WaitOne(0) }
catch [System.Threading.AbandonedMutexException] { $haveMutex = $true }   # previous holder died: it's ours now
if (-not $haveMutex) {
    if (-not $Scheduled) { Write-Host 'Another backup (or review) is already running.' -ForegroundColor Yellow }
    $mutex.Dispose()
    exit 3
}

try {
    New-Item -ItemType Directory -Force -Path $LocalLogDir, $StateDir | Out-Null

    # A run that was still "running" in status.json was interrupted (killed, reboot, logoff)
    $oldCurrent = Initialize-Status
    if ($oldCurrent -and (Get-JsonProp $oldCurrent 'state') -eq 'running') {
        $script:St.current = [ordered]@{}
        foreach ($p in $oldCurrent.PSObject.Properties) { $script:St.current[$p.Name] = $p.Value }
        $hb = Get-JsonProp $oldCurrent 'heartbeatUtc'
        $when = ''
        if ($hb) { $when = ' (last activity ' + ([DateTimeOffset]::FromUnixTimeMilliseconds([int64]$hb).LocalDateTime.ToString('dd.MM. HH:mm')) + ')' }
        $savedW = $script:Warnings; $savedE = $script:Errors
        $script:Warnings = [int](Get-JsonProp $oldCurrent 'warnings'); $script:Errors = [int](Get-JsonProp $oldCurrent 'errors')
        Complete-Run 'interrupted' -1 "Interrupted - computer restarted or the process was ended$when"
        $script:Warnings = $savedW; $script:Errors = $savedE
    }

    $automatic = $Scheduled -and -not ($Force -or $DryRun -or $Report -or $Review)
    $due = 'due'
    if ($automatic) {
        if (Test-Paused) { $due = 'paused' } else { $due = Test-ScheduledRunDue }
    }

    if ($due -ne 'due') {
        # Paused, too early today, or today's backup is already done: nothing to do, no log
        Set-LastCheck $due
        $exitCode = 0
    }
    elseif (-not (Test-DirectoryExists $TargetRoot)) {
        if (-not $automatic) { $script:LogFile = Join-Path $LocalLogDir "$Stamp.log" }   # automatic runs retry every 30 min
        Write-Log "NAS not reachable ($TargetRoot) - nothing done." 'WARN'
        Set-LastCheck 'nas_unreachable'
        Test-BackupAge
        $exitCode = 1
    }
    else {
        if ($DryRun) { $logDir = $LocalLogDir } else { $logDir = Join-Path $TargetRoot $LogsDirName }
        New-Directory $logDir
        $script:LogFile = Join-Path $logDir "$Stamp.log"
        Remove-Item -LiteralPath $StopFile -ErrorAction SilentlyContinue     # an old stop request must not stop this run

        if ($Review) {
            Invoke-Review
        }
        else {
            $script:St.current = [ordered]@{
                state = 'running'; runId = $Stamp; mode = $Mode; trigger = $Trigger; phase = 'starting'
                startedUtc = (Get-NowMs); heartbeatUtc = (Get-NowMs)
                folder = ''; folderIndex = 0; folderTotal = @($cfg.Folders).Count
                filesPlanned = 0; filesDone = 0; bytesPlanned = [int64]0; bytesDone = [int64]0; bytesPerSec = [int64]0
                currentFile = ''; versioned = 0; warnings = 0; errors = 0; logFile = $script:LogFile
            }
            Set-LastCheck 'started'

            if ($Report) {
                $null = Invoke-OrphanReport -LogDir $logDir
                Complete-Run 'ok' 0 ''
            }
            else {
                $modeText = if ($DryRun) { ' (DRY RUN - nothing is changed)' } else { '' }
                Write-Log "Backup started$modeText -> $TargetRoot"
                $sw = [Diagnostics.Stopwatch]::StartNew()
                Set-KeepAwake $true
                try { Invoke-Backup -LogDir $logDir } finally { Set-KeepAwake $false }
                $script:St.current.warnings = $script:Warnings
                $script:St.current.errors = $script:Errors
                Write-Log ('Backup finished after {0:hh\:mm\:ss} - {1} file(s) copied, {2} warning(s), {3} error(s)' -f $sw.Elapsed, $script:St.current.filesDone, $script:Warnings, $script:Errors)

                if ($script:Stopped) {
                    Complete-Run 'stopped' 4 'Stopped by you - files copied so far are kept'
                    Remove-Item -LiteralPath $StopFile -ErrorAction SilentlyContinue
                    $exitCode = 4
                }
                else {
                    if ($script:Errors -gt 0) { $result = 'errors'; $exitCode = 2 }
                    elseif ($script:Warnings -gt 0) { $result = 'warnings'; $exitCode = 1 }
                    else { $result = 'ok' }

                    if (-not $DryRun) {
                        if ($script:Errors -eq 0) { Set-State 'last-success' (Get-Date -Format o) }
                        $message = ''
                        if ($result -eq 'errors') { $message = "$($script:Errors) error(s) - see the log" }
                        elseif ($result -eq 'warnings') { $message = "$($script:Warnings) warning(s), e.g. files open in another program" }

                        # weekly: list files that exist only on the NAS (before Complete-Run, so it shows as part of the run)
                        $lastReport = Get-State 'last-orphan-report'
                        $reportDue = (-not $lastReport) -or (((Get-Date) - [datetime]::Parse($lastReport)).TotalDays -ge $cfg.OrphanReportEveryDays)
                        $count = 0
                        if ($reportDue) { try { $count = Invoke-OrphanReport -LogDir $logDir -Quiet } catch { Write-Log "NAS-only check failed: $($_.Exception.Message)" 'INFO' } }

                        Complete-Run $result $exitCode $message
                        Remove-OldLogs $logDir

                        if ($count -gt 0) { Show-Toast 'Hamster Backup' "$count item(s) are no longer on this PC. Open Hamster -> Review to decide." }
                        if ($result -eq 'errors') {
                            Show-ToastOncePerDay 'last-error-toast' 'Hamster Backup: error' "Backup had $($script:Errors) error(s). It retries automatically."
                        }
                    }
                    else {
                        Complete-Run $result $exitCode ''
                    }
                }
            }
        }
    }
}
catch {
    Write-Log "Unexpected error: $($_.Exception.Message)" 'ERROR'
    if ($script:St.current) { Complete-Run 'failed' 2 $_.Exception.Message }
    Show-ToastOncePerDay 'last-error-toast' 'Hamster Backup failed' $_.Exception.Message
    $exitCode = 2
}
finally {
    try { $mutex.ReleaseMutex() } catch { }
    $mutex.Dispose()
}
exit $exitCode
