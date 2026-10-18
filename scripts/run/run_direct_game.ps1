param(
    [Parameter(Mandatory)][string]$Apk,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9_.]+$')][string]$Package,
    [Parameter(Mandatory)][string]$Activity,
    [string]$AssetList,
    [string]$GameName,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$DataDirectory,
    [Parameter(Mandatory)][string]$Sysroot,
    [string]$HostExe,
    [string]$GpuLayerDirectory,
    [ValidateSet('dynarmic', 'digitalis')][string]$Translator = 'dynarmic',
    # ASTC storage: uncompressed RGBA8, GPU BC3, or CPU BC7.
    [ValidateSet('rgba8', 'gpu', 'bc7')][string]$TextureMode = 'rgba8',
    # Dynarmic's faster floating point (AXRB_DIRECT_FAST_FP); no effect with Digitalis.
    [switch]$FastFp,
    # Guest CPU count (AXRB_DIRECT_CPUS); 0 keeps the runner's own choice, up to 8.
    [ValidateRange(0, 64)][int]$GuestCpus = 0,
    # Multiplies the recommended eye size reported to the game (per-game compatibility).
    [ValidateRange(0.5, 2.0)][double]$RenderScale = 1.0,
    [switch]$PrecomposeProjectionLayers,
    # Per-game compatibility for unmodified Quest APKs with older Oculus loaders.
    [switch]$LegacyOculusDriver,
    [switch]$QuestIdentity,
    [switch]$ControllersOnly,
    # OpenGL ES driver: ANGLE (on the forwarded Vulkan driver), the stub with no
    # display, or auto: ANGLE for Unreal games built without Vulkan.
    # AXRB_DIRECT_GLES overrides auto.
    [ValidateSet('auto', 'angle', 'stub')][string]$Gles = 'auto',
    [ValidatePattern('^Local\\AXRB\.FpsHud\.[a-f0-9]{32}$')][string]$FpsHudEventName,
    [ValidatePattern('^[a-f0-9]{8}$')][string]$SessionId
)
# Direct mode: the game runs in axrb-direct.exe (no Android emulator) and
# renders through the same host bridge as an emulator session. The session
# ends when the bridge window closes (the launcher's Stop asks for exactly
# that), when the game exits, or when either process is lost. Ending asks the
# game to leave its activity first, so it can save.
# Output conventions match run_windows_game.ps1, which the launcher also runs.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
trap { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }
. "$PSScriptRoot/../paths.ps1"
if (!$HostExe) { $HostExe = $AxrbHostExe }
if (!$GpuLayerDirectory) { $GpuLayerDirectory = $AxrbGpuDirectory }
$HostExe = (Resolve-Path -LiteralPath $HostExe).Path
foreach ($required in @($Apk, $Executable, (Join-Path $Sysroot 'system/lib64'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "Direct mode is missing $required." }
}
if ([string]::IsNullOrWhiteSpace($GameName)) { $GameName = $Package }
$logs = Join-Path $AxrbOut 'logs/game'
New-Item -ItemType Directory -Force -Path $logs | Out-Null

$python = Join-Path $AxrbRoot 'tools/python/python.exe'
if (!(Test-Path -LiteralPath $python)) { $python = (Get-Command python -ErrorAction Stop).Source }
$directDirectory = Split-Path -Parent $Executable
$prepare = @((Join-Path $AxrbRoot 'direct/tools/prepare_art_session.py'),
    '--apk', $Apk, '--activity', $Activity, '--sysroot', $Sysroot, '--data', $DataDirectory,
    '--bootstrap', (Join-Path $directDirectory 'art/axrb-bootstrap.dex'),
    '--native', (Join-Path $directDirectory 'art/lib64'),
    '--proxies', (Join-Path $directDirectory 'guest/lib64'), '--textures', $TextureMode,
    '--render-scale', $RenderScale.ToString([Globalization.CultureInfo]::InvariantCulture))
if ($AssetList) { $prepare += @('--assets', $AssetList) }
if ($LegacyOculusDriver) { $prepare += @('--legacy-oculus-driver', (Join-Path $directDirectory 'guest/axrb-systemdriver.apk')) }
if ($QuestIdentity) { $prepare += '--quest-identity' }
if ($ControllersOnly) { $prepare += '--controllers-only' }
if ($Gles -eq 'auto' -and $env:AXRB_DIRECT_GLES -in @('angle', 'stub')) { $Gles = $env:AXRB_DIRECT_GLES }
$prepare += @('--gles', $Gles)
$prepared = & $python @prepare
if ($LASTEXITCODE -ne 0) { throw 'Preparing the AOSP application failed. See the error above.' }
$arguments = $prepared | ConvertFrom-Json
$glesDriver = if ($arguments -contains 'ro.hardware.egl=angle') { 'angle' } else { 'stub' }

function Quote([string]$Value) {
    # One Windows command-line argument, including embedded quotes and trailing backslashes.
    '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

$closeEventName = 'Local\AXRB.Close.' + [guid]::NewGuid().ToString('N')
$closeRequest = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::ManualReset, $closeEventName)
$closeReady = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::ManualReset, "$closeEventName.ready")
$stopEventName = 'Local\AXRB.DirectStop.' + [guid]::NewGuid().ToString('N')
$stopRequest = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::ManualReset, $stopEventName)
$saved = @{}
try {
    $environment = @{
        AXRB_CLOSE_EVENT = $closeEventName
        AXRB_FPS_HUD_EVENT = $FpsHudEventName
        AXRB_PRECOMPOSE_PROJECTION_LAYERS = $(if ($PrecomposeProjectionLayers) { '1' } else { '0' })
        # The bridge's wait for a fresher game frame, on a high-resolution
        # timer: its default condition variable can overshoot by a 15.6 ms
        # timer tick and miss the display deadline. AXRB_PRECISE_IMAGE_WAIT=0
        # in the environment keeps the default.
        AXRB_PRECISE_IMAGE_WAIT = $(if ($env:AXRB_PRECISE_IMAGE_WAIT) { $env:AXRB_PRECISE_IMAGE_WAIT } else { '1' })
    }
    foreach ($name in $environment.Keys) { $saved[$name] = [Environment]::GetEnvironmentVariable($name); [Environment]::SetEnvironmentVariable($name, $environment[$name]) }
    $sessionStartedAt = Get-Date
    $bridgeProcess = Start-Process -FilePath $HostExe -ArgumentList @('--serve-openxr', '38490', '0', (Quote $GameName)) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\host.log" -RedirectStandardError "$logs\host.err"
    $null = $bridgeProcess.Handle  # keeps its exit code readable after it exits
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    $saved = @{}

    # The game reads the eye size from the bridge at startup, so it starts once
    # the bridge's pose server listens.
    $ready = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while ([DateTime]::UtcNow -lt $deadline -and !$bridgeProcess.HasExited) {
        if (Test-Path -LiteralPath "$logs\host.err") {
            if (Select-String -LiteralPath "$logs\host.err" -SimpleMatch 'AXRB TCP: listening on 0.0.0.0:38490' -Quiet -ErrorAction SilentlyContinue) { $ready = $true; break }
            if (Select-String -LiteralPath "$logs\host.err" -Pattern 'AXRB TCP: (socket creation failed|bind failed on port 38490|listen failed)' -Quiet -ErrorAction SilentlyContinue) { throw "Host pose server failed to listen. Full log: $logs\host.err" }
        }
        Start-Sleep -Milliseconds 100
    }
    if (!$ready) {
        $reason = ''
        if (Test-Path -LiteralPath "$logs\host.err") { $reason = ([string]((Get-Content -LiteralPath "$logs\host.err" -Tail 6 -ErrorAction SilentlyContinue) -join ' ')).Trim() }
        if (!$reason) { $reason = 'SteamVR or another OpenXR runtime with a connected headset is required.' }
        throw "Host failed to start: $reason Full log: $logs\host.err"
    }

    $environment = @{
        AXRB_DIRECT_STOP_EVENT = $stopEventName
        AXRB_DIRECT_TRANSLATOR = $Translator
        AXRB_DIRECT_LOG = Join-Path $logs 'direct.log'
        AXRB_GPU_LAYER_DIR = $GpuLayerDirectory
    }
    # Set only when chosen, so a value in the user's own environment still applies otherwise.
    if ($FastFp) { $environment.AXRB_DIRECT_FAST_FP = '1' }
    if ($GuestCpus -gt 0) { $environment.AXRB_DIRECT_CPUS = [string]$GuestCpus }
    foreach ($name in $environment.Keys) { $saved[$name] = [Environment]::GetEnvironmentVariable($name); [Environment]::SetEnvironmentVariable($name, $environment[$name]) }
    $gameProcess = Start-Process -FilePath $Executable -ArgumentList @($arguments | ForEach-Object { Quote $_ }) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\direct.out" -RedirectStandardError "$logs\direct.err"
    $null = $gameProcess.Handle
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    $saved = @{}
    Write-Output "$GameName | AXRB direct mode is running. Closing its window stops this game session."

    while (!$bridgeProcess.HasExited -and !$gameProcess.HasExited) {
        if ($closeRequest.WaitOne(0)) { $closeRequested = $true; break }
        Start-Sleep -Milliseconds 250
    }
    if (!$closeRequested -and $closeRequest.WaitOne(0)) { $closeRequested = $true }
    if ($gameProcess.HasExited) {
        $gameExit = [ordered]@{ kind = 'exited'; reason = $null; status = $gameProcess.ExitCode }
        Write-Output "Game process ended (exit code $($gameProcess.ExitCode))."
    } elseif (!$closeRequested) {
        # SteamVR's "Exit game" ends the bridge (the app SteamVR knows), which
        # is a normal way to stop. Only a Windows exception status (0xC0000005
        # and the like, negative as Int32) is a crash.
        $hostExitCode = $bridgeProcess.ExitCode
        if ($hostExitCode -lt 0) { $hostLost = $true }
        else { $hostClosed = $true; Write-Output "The OpenXR runtime closed the AXRB host (exit code $hostExitCode); stopping the game." }
    }
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    if ($gameProcess -and !$gameProcess.HasExited) {
        # The game pauses, stops and destroys its activity, then exits; the
        # bridge stays up meanwhile so rendering and input keep working.
        $null = $stopRequest.Set()
        if ($gameProcess.WaitForExit(25000)) {
            $pauseSucceeded = Select-String -LiteralPath "$logs\direct.out" -SimpleMatch 'AXRB_AOSP_ACTIVITIES_STOPPED' -Quiet -ErrorAction SilentlyContinue
            if ($pauseSucceeded) { Write-Output 'Game activities stopped before shutdown.' }
            else { Write-Output 'Game exited before confirming activity shutdown.' }
        } else {
            Write-Output 'Warning: the game did not close within 25 seconds; stopping it.'
            $gameProcess.Kill()
            $null = $gameProcess.WaitForExit(5000)
        }
    } elseif ($gameProcess) { $pauseSucceeded = $gameProcess.ExitCode -eq 0 }
    if ($closeReady) { $null = $closeReady.Set() }
    if ($bridgeProcess -and !$bridgeProcess.HasExited) {
        $null = $bridgeProcess.CloseMainWindow()
        if (!$bridgeProcess.WaitForExit(5000)) { $bridgeProcess.Kill() }
    }
    foreach ($item in @($gameProcess, $bridgeProcess, $closeRequest, $closeReady, $stopRequest)) { if ($item) { $item.Dispose() } }
    $session = [ordered]@{
        id = $SessionId
        backend = 'direct'
        package = $Package; activity = $Activity; game = $GameName
        startedAt = if ($sessionStartedAt) { $sessionStartedAt.ToString('o') } else { $null }
        endedAt = (Get-Date).ToString('o')
        closeRequested = [bool]$closeRequested
        diagnosticDiskGuard = $false
        gameProcessLost = $false
        gameExit = $gameExit
        hostProcessLost = [bool]$hostLost
        hostClosed = [bool]$hostClosed
        hostExitCode = $hostExitCode
        pauseSucceeded = [bool]$pauseSucceeded
        # Saves are ordinary Windows files, written as the game writes them.
        syncSucceeded = [bool]$pauseSucceeded
        # What this session ran with, so logs from different settings can be told apart.
        settings = [ordered]@{
            translator = $Translator; textureMode = $TextureMode; fastFp = [bool]$FastFp
            guestCpus = $(if ($GuestCpus -gt 0) { $GuestCpus } else { 'auto' })
            renderScale = $RenderScale
            gles = $glesDriver
            preciseImageWait = $(if ($env:AXRB_PRECISE_IMAGE_WAIT) { $env:AXRB_PRECISE_IMAGE_WAIT } else { '1' })
            precomposeProjectionLayers = [bool]$PrecomposeProjectionLayers
            legacyOculusDriver = [bool]$LegacyOculusDriver; questIdentity = [bool]$QuestIdentity; controllersOnly = [bool]$ControllersOnly
        }
        build = $(try {
            $info = Join-Path (Split-Path -Parent $Executable) 'build-info.json'
            $record = if (Test-Path -LiteralPath $info) { Get-Content -LiteralPath $info -Raw | ConvertFrom-Json } else { [ordered]@{} }
            $record | Add-Member -NotePropertyName runnerSha256 -NotePropertyValue ((Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.Substring(0, 16).ToLowerInvariant()) -Force -PassThru
        } catch { $null })
    }
    try { $session | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $logs 'session.json') -Encoding UTF8 } catch { Write-Output "Warning: could not write session record: $_" }
    # The next session overwrites these logs; keep a copy of the last ten
    # sessions in logs/game/sessions/<start time>-<package>. direct.log holds
    # direct.out and direct.err, so those are not copied.
    try {
        $history = Join-Path $logs 'sessions'
        $stamp = if ($sessionStartedAt) { $sessionStartedAt.ToString('yyyyMMdd-HHmmss') } else { (Get-Date).ToString('yyyyMMdd-HHmmss') }
        $kept = Join-Path $history "$stamp-$Package"
        New-Item -ItemType Directory -Force -Path $kept | Out-Null
        foreach ($name in 'session.json', 'direct.log', 'host.err', 'host.log') {
            $file = Join-Path $logs $name
            if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination $kept }
        }
        Get-ChildItem -LiteralPath $history -Directory | Sort-Object Name -Descending | Select-Object -Skip 10 |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
    } catch { Write-Output "Warning: could not keep this session's logs: $_" }
    Write-Output 'AXRB game session stopped.'
}
if ($hostLost) { exit 4 }
