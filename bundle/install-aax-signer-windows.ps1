# Sets up AAX signing on Samuel's PC (the one the iLok moves to after the Mac):
#   - the `sign-aax` command (%LOCALAPPDATA%\SammyJ\bin\sign-aax.cmd, added to your user PATH): runs the latest
#     bundle/sign-aax-windows.ps1 from main, which signs the Windows AAX of every release waiting at
#     "Sign on the PC" (or the plug-ins you name);
#   - a prompt (the scheduled task "Sammy J AAX signing prompt", every 2 minutes while you're logged on) that
#     asks once per release when signing is waiting: move the iLok here, then Sign or Later. It never signs
#     without you.
#
#   powershell -ExecutionPolicy Bypass -File bundle\install-aax-signer-windows.ps1              install, or update
#   powershell -ExecutionPolicy Bypass -File bundle\install-aax-signer-windows.ps1 -Uninstall
#
# Needs what bundle/sign-aax-windows.ps1 needs (gh logged in, PACE's tools, %APPDATA%\SammyJ\pace.json, the
# signing certificate). Log: %LOCALAPPDATA%\SammyJ\Logs\AAX-Signer.log. The PC's twin of
# bundle/install-aax-signer.sh. Works in Windows PowerShell 5.1.
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$taskName = 'Sammy J AAX signing prompt'
$dir = Join-Path $env:LOCALAPPDATA 'SammyJ\aax-signer'
$bin = Join-Path $env:LOCALAPPDATA 'SammyJ\bin'
$log = Join-Path $env:LOCALAPPDATA 'SammyJ\Logs\AAX-Signer.log'
$agent = Join-Path $dir 'aax-signer-agent-windows.ps1'
$launcher = Join-Path $dir 'aax-signer-agent.vbs'
$runner = Join-Path $dir 'sign-aax.ps1'
$cmd = Join-Path $bin 'sign-aax.cmd'

function Set-UserPath([string]$Folder, [bool]$Present) {
  $current = [Environment]::GetEnvironmentVariable('Path', 'User')
  $parts = @(if ($current) { $current.Split(';') | Where-Object { $_ -and $_.TrimEnd('\') -ne $Folder.TrimEnd('\') } })
  if ($Present) { $parts += $Folder }
  $new = $parts -join ';'
  if ($new -ne $current) { [Environment]::SetEnvironmentVariable('Path', $new, 'User') }
}

if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
  Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}

if ($Uninstall) {
  Remove-Item -Force -ErrorAction SilentlyContinue $agent, $launcher, $runner, $cmd
  Set-UserPath $bin $false
  Write-Host "Removed sign-aax and its prompt (%APPDATA%\SammyJ\pace.json and the log are left alone)."
  exit 0
}

New-Item -ItemType Directory -Force $dir, $bin, (Split-Path $log) | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'aax-signer-agent-windows.ps1') $agent -Force

# sign-aax.ps1: fetches the current signing script and plug-in list from main each time, runs it and logs.
# SAMMYJ_AAX_REF=<branch> fetches them from that branch instead (for testing changes before they're merged).
$runnerText = @'
# sign-aax: signs the Windows AAX of releases waiting for this PC. Runs the current
# bundle/sign-aax-windows.ps1 from samueljustice/plugins' main branch (see its header for options).
# Installed by bundle/install-aax-signer-windows.ps1.
$repo = 'samueljustice/plugins'
$ref = if ($env:SAMMYJ_AAX_REF) { $env:SAMMYJ_AAX_REF } else { 'main' }
$log = Join-Path $env:LOCALAPPDATA 'SammyJ\Logs\AAX-Signer.log'
$work = Join-Path ([IO.Path]::GetTempPath()) ('sammyj-sign-aax-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force (Join-Path $work 'bundle'), (Split-Path $log) | Out-Null
try {
  foreach ($f in 'sign-aax-windows.ps1', 'plugins.txt') {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = 'gh'
    $psi.Arguments = "api ""repos/$repo/contents/bundle/${f}?ref=$ref"" -H ""Accept: application/vnd.github.raw"""
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $p = $null
    try { $p = [Diagnostics.Process]::Start($psi) } catch {}
    $text = $null
    if ($p) { $text = $p.StandardOutput.ReadToEndAsync(); $null = $p.StandardError.ReadToEndAsync(); $p.WaitForExit() }
    if (-not $p -or $p.ExitCode -ne 0) {
      Write-Host "Couldn't fetch bundle/$f from GitHub (offline, or gh not logged in?)"
      exit 1
    }
    [IO.File]::WriteAllText((Join-Path $work "bundle\$f"), $text.Result, (New-Object Text.UTF8Encoding $true))
  }
  Set-Location (Join-Path $env:LOCALAPPDATA 'SammyJ\aax-signer')
  Add-Content -Path $log -Value "== $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') sign-aax $($args -join ' ')" -Encoding UTF8
  & (Join-Path $work 'bundle\sign-aax-windows.ps1') @args *>&1 | ForEach-Object {
    $line = "$_"
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding UTF8
  }
  exit $LASTEXITCODE
} finally {
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
'@
[IO.File]::WriteAllText($runner, $runnerText, (New-Object Text.UTF8Encoding $true))

[IO.File]::WriteAllText($cmd, ("@powershell.exe -NoProfile -ExecutionPolicy Bypass -File ""$runner"" %*`r`n" +
                               "@exit /b %ERRORLEVEL%`r`n"), (New-Object Text.ASCIIEncoding))

# wscript starts the prompt without flashing a console window every 2 minutes.
[IO.File]::WriteAllText($launcher, ("CreateObject(""WScript.Shell"").Run ""powershell.exe -NoProfile -ExecutionPolicy Bypass " +
                                    "-WindowStyle Hidden -File """"$agent"""""", 0, True`r`n"), (New-Object Text.ASCIIEncoding))

$user = "$env:USERDOMAIN\$env:USERNAME"
$every = (New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 2)).Repetition
$atLogOn = New-ScheduledTaskTrigger -AtLogOn -User $user
$atLogOn.Repetition = $every
$fromNow = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes 2)
$action = New-ScheduledTaskAction -Execute 'wscript.exe' -Argument "//B //Nologo ""$launcher"""
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -StartWhenAvailable -AllowStartIfOnBatteries `
              -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 2)
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger @($atLogOn, $fromNow) -Principal $principal `
  -Settings $settings -Description 'Asks to sign AAX plug-ins when a release is waiting at "Sign on the PC" (samueljustice/plugins).' | Out-Null

Set-UserPath $bin $true
if (($env:Path.Split(';') | ForEach-Object { $_.TrimEnd('\') }) -notcontains $bin.TrimEnd('\')) { $env:Path += ";$bin" }

Write-Host "Installed sign-aax ($cmd) and its prompt (scheduled task ""$taskName"")."
Write-Host "Open a new terminal to type sign-aax anywhere ($bin is now on your user PATH)."
Write-Host "Log: $log    Remove with: powershell -ExecutionPolicy Bypass -File $PSCommandPath -Uninstall"
