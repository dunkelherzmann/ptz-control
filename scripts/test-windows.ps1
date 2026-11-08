# Tests PTZ Control for Windows against the mock camera without touching the user's settings or saved logins.
#  1. The core's login vault: tests\vault-smoke.c built with MSVC and run.
#  2. The app (dist\PTZControl.exe) twice, with its own registry key and vault file, never taking focus:
#     run 1 types a login, ticks Save login and connects; run 2 starts with a wrong username and fills the
#     camera fields from the saved login. Each run saves pictures in .build\test.
# Usage: scripts\test-windows.ps1   (after scripts\build-windows.ps1)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Out = Join-Path $Root '.build\test'
$Work = Join-Path $Root ('.build\test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $Out, $Work | Out-Null
$Fail = $false
function Check($what, $ok) {
    if ($ok) { Write-Output "ok   $what" } else { Write-Output "FAIL $what"; $script:Fail = $true }
}

# ---- 1. core vault test ------------------------------------------------------------------
$vcvars = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
$obj = Join-Path $Work 'obj'
New-Item -ItemType Directory -Force -Path $obj | Out-Null
$core = @('vault.cpp', 'monocypher.cpp', 'util.cpp') | ForEach-Object { "`"$Root\core\src\$_`"" }
$cmd = "`"$vcvars`" >nul && cd /d `"$obj`" && cl /nologo /std:c++20 /EHsc /O1 /utf-8 /I`"$Root\core\include`" /c $($core -join ' ') >nul " +
       "&& cl /nologo /utf-8 /I`"$Root\core\include`" /c `"$Root\tests\vault-smoke.c`" >nul " +
       "&& link /nologo vault.obj monocypher.obj util.obj vault-smoke.obj bcrypt.lib ws2_32.lib /OUT:vault-smoke.exe >nul"
cmd /c $cmd
$vaultDir = Join-Path $Work 'vault'
New-Item -ItemType Directory -Force -Path $vaultDir | Out-Null
$result = & "$obj\vault-smoke.exe" $vaultDir 2>&1
Write-Output $result
Check 'core vault test' ($LASTEXITCODE -eq 0)

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
    RunApp @{ PTZ_PASSWORD = 'secret'; PTZ_SNAPSHOT_PATH = $shot; PTZ_SNAPSHOT_SAVE_LOGIN = '1' }
    Check 'run 1 connected and moved' ((Moves) -ge 1)
    Check 'run 1 saved the camera login' ((Get-Content $vault) -contains "camera`tadmin`tsecret")
    Check 'window picture saved' ((Test-Path $shot) -and (Get-Item $shot).Length -gt 0)
    $before = Moves

    Settings 'nobody'
    $logins = Join-Path $Out 'windows-logins.png'
    Remove-Item $logins -ErrorAction SilentlyContinue
    RunApp @{ PTZ_PASSWORD = $null; PTZ_SNAPSHOT_PATH = (Join-Path $Out 'windows-saved-login.png'); PTZ_SNAPSHOT_USE_LOGIN = '1';
              PTZ_SNAPSHOT_LOGINS_PATH = $logins }
    Check 'run 2 connected with the saved login and moved' ((Moves) -gt $before)
    Check 'Manage Logins picture saved' ((Test-Path $logins) -and (Get-Item $logins).Length -gt 0)
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
