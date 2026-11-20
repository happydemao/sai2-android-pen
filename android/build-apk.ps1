<#
    SAI-on-Winlator pen-pressure patch builder (no Gradle).

    Pipeline: base.apk classes.dex -> baksmali -> smali patches -> smali assemble
              -> javac + d8 for PenBridge -> classes2.dex
              -> repack base.apk with both dex -> zipalign -> apksigner -> optional adb install -r

    Usage:
        .\build-apk.ps1              # build only
        .\build-apk.ps1 -Install     # build and install (same signature, container data kept)

    NOTE: keep this file ASCII-only. Windows PowerShell 5.1 parses UTF-8 without BOM as ANSI
    and will fail on CJK comments.
#>
[CmdletBinding()]
param(
    [switch]$Install,
    [string]$Adb = '',

    # Inputs. Defaults follow the local build layout (<repo>/.stage, <repo>/android-sdk);
    # override any of them, or set the matching environment variable.
    [string]$BaseApk = '',
    [string]$SmaliTools = '',
    [string]$Keystore = '',
    [string]$Sdk = '',
    [string]$Jdk = '',
    [string]$OutApk = ''
)

$ErrorActionPreference = 'Stop'

$here  = $PSScriptRoot              # this script lives in <repo>/android/
$repo  = Split-Path $here -Parent
$stage = Join-Path $repo '.stage'   # build scratch, never committed

if (-not $BaseApk)    { $BaseApk    = $env:SAI_BASE_APK;    if (-not $BaseApk)    { $BaseApk    = Join-Path $stage 'base.apk' } }
if (-not $SmaliTools) { $SmaliTools = $env:SAI_SMALI_TOOLS; if (-not $SmaliTools) { $SmaliTools = Join-Path $stage 'smali-tools' } }
if (-not $Keystore)   { $Keystore   = $env:SAI_KEYSTORE;    if (-not $Keystore)   { $Keystore   = Join-Path $stage 'debug.keystore' } }
if (-not $Sdk)        { $Sdk        = $env:ANDROID_SDK_ROOT; if (-not $Sdk)       { $Sdk        = Join-Path $repo 'android-sdk' } }
if (-not $Jdk)        { $Jdk        = $env:JAVA_HOME;       if (-not $Jdk)        { $Jdk        = Join-Path $repo 'tools-jdk\jdk-21.0.13.0.2+1' } }
if (-not $OutApk)     { $OutApk     = Join-Path $repo 'deliverables\Winlator-11.2-SAIPen.apk' }

$baseApk    = $BaseApk
$keystore   = $Keystore
$tools      = $SmaliTools
$sdk        = $Sdk
$jdk        = $Jdk
$bt         = Join-Path $sdk 'build-tools\36.0.0'
$androidJar = Join-Path $sdk 'platforms\android-35\android.jar'

$smaliDir = Join-Path $stage 'smali-clean'
$classDir = Join-Path $stage 'penbridge-classes'
$outApk   = $OutApk

$env:JAVA_HOME = $jdk
$env:PATH = "$jdk\bin;$bt;$env:PATH"

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail($msg) { throw "FAILED: $msg" }

function Invoke-Tool([string]$exe, [string[]]$argList, [string]$label) {
    & $exe @argList
    if ($LASTEXITCODE -ne 0) { Fail "$label (exit $LASTEXITCODE)" }
}

function Remove-DirSafe([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    $full = (Resolve-Path -LiteralPath $path).Path
    try { [System.IO.Directory]::Delete("\\?\$full", $true) } catch { }
    if (Test-Path -LiteralPath $path) { & cmd.exe /c "rd /s /q `"$full`"" | Out-Null }
    if (Test-Path -LiteralPath $path) { Fail "cannot remove $path" }
}

function NormalizeLF([string]$s) { return $s.Replace("`r`n", "`n") }

function Patch-Once([string]$text, [string]$old, [string]$new, [string]$label) {
    $old = NormalizeLF $old
    $new = NormalizeLF $new
    $i = $text.IndexOf($old)
    if ($i -lt 0) { Fail "patch anchor not found: $label" }
    if ($text.IndexOf($old, $i + 1) -ge 0) { Fail "patch anchor not unique: $label" }
    return $text.Substring(0, $i) + $new + $text.Substring($i + $old.Length)
}

if ($Install -and -not $Adb) {
    $found = Get-ChildItem (Join-Path ([Environment]::GetFolderPath('Desktop')) '*\bins\adb.exe') -ErrorAction SilentlyContinue
    if ($found) { $Adb = $found[0].FullName }
    if (-not $Adb) { Fail 'adb not found; pass -Adb <path>' }
}

# ---------------------------------------------------------------- 0. preflight
foreach ($p in @($baseApk, $keystore, $androidJar,
                (Join-Path $tools 'baksmali-2.5.2.jar'),
                (Join-Path $tools 'smali-2.5.2.jar'),
                (Join-Path $bt 'zipalign.exe'),
                (Join-Path $bt 'apksigner.bat'),
                (Join-Path $bt 'd8.bat'))) {
    if (-not (Test-Path $p)) {
        Fail "missing: $p`n  -> pass one of -BaseApk / -SmaliTools / -Keystore / -Sdk / -Jdk, see README '自己构建'."
    }
}

# ------------------------------------------- 1. pristine classes.dex from base.apk
Step 'Extract pristine classes.dex from base.apk'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($baseApk)
$entry = $zip.Entries | Where-Object { $_.FullName -eq 'classes.dex' }
if (-not $entry) { $zip.Dispose(); Fail 'base.apk has no classes.dex' }
$baseDex = Join-Path $stage 'base-classes.dex'
$in = $entry.Open()
$out = [System.IO.File]::Create($baseDex)
$in.CopyTo($out)
$out.Close(); $in.Close(); $zip.Dispose()

# --------------------------------------------------------------- 2. disassemble
Step 'baksmali'
Remove-DirSafe $smaliDir
Invoke-Tool 'java' @('-jar', (Join-Path $tools 'baksmali-2.5.2.jar'),
                     'd', $baseDex, '-o', $smaliDir) 'baksmali'

# ------------------------------------------------------------- 3. smali patches
Step 'Patch TouchpadView.smali'
$tp = Join-Path $smaliDir 'com\winlator\widget\TouchpadView.smali'
$t = NormalizeLF (Get-Content $tp -Raw -Encoding UTF8)

$call = @'
    # SAI pen: pressure sampling + stylus left-click synthesis.
    # All logic lives in PenBridge.handle(); no smali branches (a hand-written branch
    # here caused a VerifyError and made the container FATAL on launch).
    invoke-static {p0, p1}, Lcom/winlator/sai/PenBridge;->handle(Landroid/view/View;Landroid/view/MotionEvent;)V
'@

# A. updateXform: tell the bridge the container resolution; force absolute cursor tracking.
$t = Patch-Once $t @'
    .param p4, "innerHeight"    # I

    .line 77
'@ @'
    .param p4, "innerHeight"    # I

    # SAI pen A
    invoke-static {p3, p4}, Lcom/winlator/sai/PenBridge;->configure(II)V

    const/4 v0, 0x1

    iput-boolean v0, p0, Lcom/winlator/widget/TouchpadView;->moveCursorToTouchpoint:Z

    .line 77
'@ 'A updateXform'

# B. onTouchEvent entry (before the SOURCE_MOUSE early-return).
$t = Patch-Once $t @'
.method public onTouchEvent(Landroid/view/MotionEvent;)Z
    .registers 13
    .param p1, "event"    # Landroid/view/MotionEvent;
'@ (@'
.method public onTouchEvent(Landroid/view/MotionEvent;)Z
    .registers 13
    .param p1, "event"    # Landroid/view/MotionEvent;

'@ + $call) 'B onTouchEvent'

# D. onExternalMouseEvent entry (external pen hover goes here).
$t = Patch-Once $t @'
.method public onExternalMouseEvent(Landroid/view/MotionEvent;)Z
    .registers 8
    .param p1, "event"    # Landroid/view/MotionEvent;
'@ (@'
.method public onExternalMouseEvent(Landroid/view/MotionEvent;)Z
    .registers 8
    .param p1, "event"    # Landroid/view/MotionEvent;

'@ + $call) 'D onExternalMouseEvent'

# E. physical mouse wheel -> PageUp/PageDown (SAI zoom at cursor).
# Winlator's default wheel path injects X11 buttons 4/5; SAI receives that as view
# scrolling. SAI binds PageUp/PageDown to zoom, so translate the wheel in Android.
$t = Patch-Once $t @'
    .line 387
    iget-object v3, p0, Lcom/winlator/widget/TouchpadView;->xServer:Lcom/winlator/xserver/XServer;

    sget-object v4, Lcom/winlator/xserver/Pointer$Button;->BUTTON_SCROLL_DOWN:Lcom/winlator/xserver/Pointer$Button;

    invoke-virtual {v3, v4}, Lcom/winlator/xserver/XServer;->injectPointerButtonPress(Lcom/winlator/xserver/Pointer$Button;)V

    .line 388
    iget-object v3, p0, Lcom/winlator/widget/TouchpadView;->xServer:Lcom/winlator/xserver/XServer;

    sget-object v4, Lcom/winlator/xserver/Pointer$Button;->BUTTON_SCROLL_DOWN:Lcom/winlator/xserver/Pointer$Button;

    invoke-virtual {v3, v4}, Lcom/winlator/xserver/XServer;->injectPointerButtonRelease(Lcom/winlator/xserver/Pointer$Button;)V
'@ @'
    .line 387
    invoke-static {p0}, Lcom/winlator/sai/PenBridge;->zoomOut(Landroid/view/View;)V
'@ 'E wheel down -> zoom out'

$t = Patch-Once $t @'
    .line 391
    iget-object v3, p0, Lcom/winlator/widget/TouchpadView;->xServer:Lcom/winlator/xserver/XServer;

    sget-object v4, Lcom/winlator/xserver/Pointer$Button;->BUTTON_SCROLL_UP:Lcom/winlator/xserver/Pointer$Button;

    invoke-virtual {v3, v4}, Lcom/winlator/xserver/XServer;->injectPointerButtonPress(Lcom/winlator/xserver/Pointer$Button;)V

    .line 392
    iget-object v3, p0, Lcom/winlator/widget/TouchpadView;->xServer:Lcom/winlator/xserver/XServer;

    sget-object v4, Lcom/winlator/xserver/Pointer$Button;->BUTTON_SCROLL_UP:Lcom/winlator/xserver/Pointer$Button;

    invoke-virtual {v3, v4}, Lcom/winlator/xserver/XServer;->injectPointerButtonRelease(Lcom/winlator/xserver/Pointer$Button;)V
'@ @'
    .line 391
    invoke-static {p0}, Lcom/winlator/sai/PenBridge;->zoomIn(Landroid/view/View;)V
'@ 'E wheel up -> zoom in'

[System.IO.File]::WriteAllText($tp, $t, (New-Object System.Text.UTF8Encoding($false)))

# ------------------------------------ 3b. composite BLE keyboard / gamepad fix
# MCHOSE G75 V2 (and similar BLE keyboards) expose KEYBOARD|MOUSE|DPAD|JOYSTICK.
# The source-only test in ExternalController.isGameController() then classified the
# keyboard as a gamepad, so xserver.Keyboard.onKeyEvent() returned false before
# looking at a single keycode. Full alphabetic keyboards must never be gamepads.
Step 'Patch ExternalController.smali (BLE keyboard)'
$ec = Join-Path $smaliDir 'com\winlator\inputcontrols\ExternalController.smali'
$t = NormalizeLF (Get-Content $ec -Raw -Encoding UTF8)

$t = Patch-Once $t @'
    invoke-virtual {p0}, Landroid/view/InputDevice;->getName()Ljava/lang/String;
'@ @'
    # A full alphabetic keyboard must not be classified as a gamepad. Composite BLE
    # keyboards advertise JOYSTICK|DPAD sources for their media/mouse reports; the
    # source-only test below then treated the whole device as a controller, and
    # Keyboard.onKeyEvent() returned early for every key.
    invoke-virtual {p0}, Landroid/view/InputDevice;->getKeyboardType()I

    move-result v3

    const/4 v4, 0x2

    if-ne v3, v4, :cond_not_alphabetic_keyboard

    return v0

    :cond_not_alphabetic_keyboard
    invoke-virtual {p0}, Landroid/view/InputDevice;->getName()Ljava/lang/String;
'@ 'external controller keyboard guard'

[System.IO.File]::WriteAllText($ec, $t, (New-Object System.Text.UTF8Encoding($false)))

# --------------------------------------------------------------- 4. assemble
Step 'smali assemble'
$newDex = Join-Path $stage 'classes-patched.dex'
if (Test-Path $newDex) { Remove-Item $newDex -Force }
Invoke-Tool 'java' @('-jar', (Join-Path $tools 'smali-2.5.2.jar'),
                     'a', $smaliDir, '-o', $newDex) 'smali'
if (-not (Test-Path $newDex)) { Fail 'smali produced no dex (see message above)' }

# ---------------------------------------------- 5. PenBridge -> classes2.dex
Step 'javac + d8 (PenBridge)'
Remove-DirSafe $classDir
New-Item -ItemType Directory -Path $classDir | Out-Null
Invoke-Tool (Join-Path $jdk 'bin\javac.exe') @(
    '--release', '11', '-encoding', 'UTF-8',
    '-cp', $androidJar, '-d', $classDir,
    (Join-Path $here 'PenBridge.java')) 'javac'

$classFiles = Get-ChildItem $classDir -Recurse -Filter *.class | ForEach-Object { $_.FullName }
Invoke-Tool (Join-Path $bt 'd8.bat') (@('--min-api', '26', '--lib', $androidJar,
    '--output', $classDir) + $classFiles) 'd8'
$dex2 = Join-Path $classDir 'classes.dex'
if (-not (Test-Path $dex2)) { Fail 'd8 produced no classes.dex' }

# --------------------------------------------------------------- 6. repack
Step 'Repack APK'
if (-not (Test-Path (Split-Path $outApk))) { New-Item -ItemType Directory -Path (Split-Path $outApk) | Out-Null }
$unsigned = Join-Path $stage 'saipen-unsigned.apk'
Copy-Item $baseApk $unsigned -Force
$zip = [System.IO.Compression.ZipFile]::Open($unsigned, 'Update')

# NOTE: keep this file ASCII-only. PS 5.1 reads UTF-8-without-BOM as ANSI and CJK
# comments will swallow the following line, leaving variables null.
# container_pattern_patched.tzst carries the pressure bridge inside the container
# template, so every NEW container ships with wintab32.dll + wt_pmax.txt + DllOverrides.
$patternPath = Join-Path $stage 'container_pattern_patched.tzst'
Write-Host ("pattern: {0} exists={1}" -f $patternPath, (Test-Path -LiteralPath $patternPath))

foreach ($n in @('classes.dex', 'classes2.dex')) {
    $e = $zip.GetEntry($n)
    if ($e) { $e.Delete() }
}

function Add-ZipEntry($zipObj, [string]$entryName, [string]$filePath) {
    $entry = $zipObj.CreateEntry($entryName, [System.IO.Compression.CompressionLevel]::Optimal)
    $stream = $entry.Open()
    $data = [System.IO.File]::ReadAllBytes($filePath)
    $stream.Write($data, 0, $data.Length)
    $stream.Close()
}

Add-ZipEntry $zip 'classes.dex' $newDex
Add-ZipEntry $zip 'classes2.dex' $dex2

if (Test-Path -LiteralPath $patternPath) {
    $old = $zip.GetEntry('assets/container_pattern.tzst')
    if ($old) { $old.Delete() }
    Add-ZipEntry $zip 'assets/container_pattern.tzst' $patternPath
    Write-Host 'container template patched (bridge preinstalled)'
}
$zip.Dispose()

# --------------------------------------------------------------- 7. align + sign
Step 'zipalign'
$aligned = Join-Path $stage 'saipen-aligned.apk'
Invoke-Tool (Join-Path $bt 'zipalign.exe') @('-P', '16', '-f', '4', $unsigned, $aligned) 'zipalign'

Step 'apksigner'
Invoke-Tool (Join-Path $bt 'apksigner.bat') @(
    'sign', '--ks', $keystore,
    '--ks-pass', 'pass:android', '--key-pass', 'pass:android',
    '--ks-key-alias', 'androiddebugkey',
    '--v1-signing-enabled', 'true', '--v2-signing-enabled', 'true',
    '--out', $outApk, $aligned) 'apksigner'

Step 'verify signature'
Invoke-Tool (Join-Path $bt 'apksigner.bat') @('verify', '--print-certs', $outApk) 'apksigner verify'

$info = Get-Item $outApk
Write-Host ("`nOK  {0}  ({1:N1} MB)" -f $outApk, ($info.Length / 1MB)) -ForegroundColor Green

# --------------------------------------------------------------- 8. install
if ($Install) {
    Step 'adb install -r (same signature -> container data kept)'
    Invoke-Tool $Adb @('install', '-r', '-d', $outApk) 'adb install'
}
