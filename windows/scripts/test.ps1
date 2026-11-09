# Tests PTZ Control for Windows against the mock camera without touching the user's settings or saved logins.
#  1. The core's login vault: tests\vault-smoke.c built with MSVC and run.
#  2. The app (dist\PTZControl.exe) twice, with its own registry key and vault file, never taking focus:
#     run 1 types a login, ticks Save login and connects; run 2 starts with a wrong username and fills the
#     camera fields from the saved login. Each run saves pictures in .build\test.
# Usage: windows\scripts\test.ps1   (after windows\scripts\build.ps1)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Out = Join-Path $Root '.build\test'
$Work = Join-Path $Root ('.build\test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $Out, $Work | Out-Null
$Fail = $false
function Check($what, $ok) {
    if ($ok) { Write-Output "ok   $what" } else { Write-Output "FAIL $what"; $script:Fail = $true }
}

# ---- 1. core vault test ------------------------------------------------------------------
# The x64 compiler environment, found the way windows\scripts\build.ps1 finds it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
cmd /c "`"$(Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat')`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$obj = Join-Path $Work 'obj'
New-Item -ItemType Directory -Force -Path $obj | Out-Null
$flags = @('/nologo', '/O1', '/MT', '/utf-8', '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/D_WIN32_WINNT=0x0A00', '/D_CRT_SECURE_NO_WARNINGS', "/I$Root\core\include", "/Fo$obj\")
# The whole core, as test-core.sh builds it, linked with the Windows libraries the app uses.
foreach ($src in (Get-ChildItem "$Root\core\src\*.cpp")) {
    & cl.exe ($flags + @('/std:c++20', '/EHsc', '/c', $src.FullName)) 2>&1 | Where-Object { $_ -match 'error' }
}
& cl.exe ($flags + @('/c', "$Root\tests\vault-smoke.c")) 2>&1 | Where-Object { $_ -match 'error' }
& link.exe /nologo (Get-ChildItem "$obj\*.obj" | ForEach-Object FullName) winhttp.lib ws2_32.lib bcrypt.lib advapi32.lib `
    shell32.lib user32.lib "/OUT:$obj\vault-smoke.exe" 2>&1 | Where-Object { $_ -match 'error' }
Check 'core vault test built' (Test-Path "$obj\vault-smoke.exe")
$vaultDir = Join-Path $Work 'vault'
New-Item -ItemType Directory -Force -Path $vaultDir | Out-Null
if (Test-Path "$obj\vault-smoke.exe") {
    & "$obj\vault-smoke.exe" $vaultDir
    Check 'core vault test' ($LASTEXITCODE -eq 0)
}

# ---- 2. the app -----------------------------------------------------------------------------
$exe = Join-Path $Root 'dist\PTZControl.exe'
$Port = 18082
$KeyName = 'Software\PTZControl.Test'
$mockLog = Join-Path $Work 'mock.log'
$mock = Start-Process python -ArgumentList @('-u', "`"$Root\tools\mock-camera.py`"", '--port', $Port) -PassThru `
    -WindowStyle Hidden -RedirectStandardOutput $mockLog -RedirectStandardError (Join-Path $Work 'mock.err')
Start-Sleep -Seconds 1
$vault = Join-Path $Work 'logins.vault'
[IO.File]::WriteAllText($vault, "PTZ Control logins 1`nplain`n")

function Settings($user) {
    $key = "HKCU:\$KeyName"
    New-Item -Path $key -Force | Out-Null
    Set-ItemProperty $key Route 0 -Type DWord
    Set-ItemProperty $key Vendor 0 -Type DWord
    Set-ItemProperty $key CameraHost '127.0.0.1'
    Set-ItemProperty $key CameraPort $Port -Type DWord
    Set-ItemProperty $key CameraUser $user
}
# A picture counts only when something was drawn: many colors, not one flat fill.
Add-Type -AssemblyName System.Drawing
function Drawn($path) {
    if (-not (Test-Path $path)) { return $false }
    $image = [System.Drawing.Bitmap]::FromFile($path)
    try {
        $colors = @{}
        for ($x = 0; $x -lt $image.Width; $x += [Math]::Max(1, [int]($image.Width / 40))) {
            for ($y = 0; $y -lt $image.Height; $y += [Math]::Max(1, [int]($image.Height / 40))) { $colors[$image.GetPixel($x, $y).ToArgb()] = 1 }
        }
        return $colors.Count -ge 8
    } finally { $image.Dispose() }
}
function Moves { @(Select-String -Path $mockLog -Pattern '^mock: onvif AbsoluteMove' -ErrorAction SilentlyContinue).Count }
function RunApp($vars) {
    $saved = @{}
    $all = @{ PTZ_SETTINGS_KEY = $KeyName; PTZ_VAULT_PATH = $vault } + $vars
    foreach ($k in $all.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $all[$k]) }
    $p = Start-Process $exe -PassThru
    if (-not $p.WaitForExit(60000)) { $p.Kill() }
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}

try {
    Settings 'admin'
    $shot = Join-Path $Out 'windows.png'
    Remove-Item $shot -ErrorAction SilentlyContinue
    RunApp @{ PTZ_PASSWORD = 'secret'; PTZ_SNAPSHOT_PATH = $shot; PTZ_SNAPSHOT_SAVE_LOGIN = '1'; PTZ_SNAPSHOT_LOGIN_NAME = 'cam1' }
    Check 'run 1 connected and moved' ((Moves) -ge 1)
    Check 'run 1 saved the camera login' ((Get-Content $vault) -contains "camera`tcam1`tadmin`tsecret")
    Check 'window picture drawn' (Drawn $shot)
    $before = Moves

    Settings 'nobody'
    $logins = Join-Path $Out 'windows-logins.png'
    Remove-Item $logins -ErrorAction SilentlyContinue
    RunApp @{ PTZ_PASSWORD = $null; PTZ_SNAPSHOT_PATH = (Join-Path $Out 'windows-saved-login.png'); PTZ_SNAPSHOT_USE_LOGIN = '1';
              PTZ_SNAPSHOT_LOGINS_PATH = $logins }
    Check 'run 2 connected with the saved login and moved' ((Moves) -gt $before)
    Check 'Manage Logins picture drawn' (Drawn $logins)
    Check 'still one camera login' (@(Get-Content $vault | Where-Object { $_ -like 'camera*' }).Count -eq 1)
} finally {
    Stop-Process -Id $mock.Id -ErrorAction SilentlyContinue
    Remove-Item -Path "HKCU:\$KeyName" -Recurse -ErrorAction SilentlyContinue
}

if ($Fail) {
    Get-Content $mockLog -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
    exit 1
}
Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
Write-Output "PASS PTZ Control for Windows (test copy): pictures in $Out"
