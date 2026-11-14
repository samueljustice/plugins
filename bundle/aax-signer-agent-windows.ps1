# The prompt bundle/install-aax-signer-windows.ps1 sets up on Samuel's PC (a scheduled task, every 2 minutes
# while he's logged on). When a release's "AAX signing" run reaches "Sign on the PC", it asks once: move the
# iLok to this PC, then Sign (runs `sign-aax`) or Later (run `sign-aax` yourself when ready). It never signs
# on its own, because the iLok may be in the Mac. Log: %LOCALAPPDATA%\SammyJ\Logs\AAX-Signer.log
# The PC's twin of bundle/aax-signer-agent.sh. Works in Windows PowerShell 5.1.

$ErrorActionPreference = 'Stop'
$repo = 'samueljustice/plugins'
$stage = 'Sign on the PC'
$state = Join-Path $env:LOCALAPPDATA 'SammyJ\aax-signer'
$asked = Join-Path $state 'asked-runs.txt'
$log = Join-Path $env:LOCALAPPDATA 'SammyJ\Logs\AAX-Signer.log'
$signAax = Join-Path $state 'sign-aax.ps1'
New-Item -ItemType Directory -Force $state, (Split-Path $log) | Out-Null
if (-not (Test-Path $asked)) { New-Item -ItemType File $asked | Out-Null }

function Write-Log([string]$Text) { Add-Content -Path $log -Value $Text -Encoding UTF8 }

# Runs gh and returns its output lines, or $null if it failed. Builds the command line itself (Windows
# PowerShell 5.1 drops quotes inside arguments) and runs without a console window.
function Invoke-Gh([string[]]$ArgList) {
  $quoted = foreach ($a in $ArgList) {
    if ($a -ne '' -and $a -notmatch '[\s"]') { $a; continue }
    $s = '"'
    $slashes = 0
    foreach ($ch in $a.ToCharArray()) {
      if ($ch -eq '\') { $slashes++; continue }
      if ($ch -eq '"') { $s += ('\' * ($slashes * 2 + 1)) + '"' }
      else { $s += ('\' * $slashes) + $ch }
      $slashes = 0
    }
    $s + ('\' * ($slashes * 2)) + '"'
  }
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName = 'gh'
  $psi.Arguments = $quoted -join ' '
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
  try { $p = [Diagnostics.Process]::Start($psi) } catch { return $null }
  $out = $p.StandardOutput.ReadToEndAsync()
  $null = $p.StandardError.ReadToEndAsync()
  if (-not $p.WaitForExit(120000)) { try { $p.Kill() } catch {}; return $null }
  $p.WaitForExit()
  if ($p.ExitCode -ne 0) { return $null }
  @($out.Result -split "`r?`n" | Where-Object { $_ -ne '' })
}

function Show-Toast([string]$Title, [string]$Text) {
  try {
    $null = [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime]
    $null = [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime]
    $esc = { param($s) [Security.SecurityElement]::Escape($s) }
    $xml = New-Object Windows.Data.Xml.Dom.XmlDocument
    $xml.LoadXml("<toast><visual><binding template=""ToastGeneric""><text>$(& $esc $Title)</text><text>$(& $esc $Text)</text></binding></visual></toast>")
    # Windows PowerShell's own app ID, so the toast needs no app registration.
    $app = '{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe'
    [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($app).Show(
      (New-Object Windows.UI.Notifications.ToastNotification $xml))
  } catch {
    Write-Log "   (couldn't show a notification: $Title - $Text)"
  }
}

# One prompt at a time.
$mutex = New-Object Threading.Mutex($false, 'Local\SammyJ-AAX-Signer-Prompt')
if (-not $mutex.WaitOne(0)) { exit 0 }
try {
  if ($null -eq (Invoke-Gh @('auth', 'status'))) { exit 0 }   # offline or logged out: nothing to ask about

  # Runs waiting for the PC that haven't been asked about yet.
  $runs = Invoke-Gh @('run', 'list', '-R', $repo, '--workflow', 'aax-signing.yml', '--status', 'in_progress',
                      '--limit', '50', '--json', 'databaseId,displayTitle', '--jq', '.[] | "\(.databaseId)|\(.displayTitle)"')
  if (-not $runs) { exit 0 }
  $seen = @(Get-Content $asked)
  $newRuns = @()
  $releases = @()
  foreach ($r in $runs) {
    $run, $title = $r.Split('|', 2)
    if ($seen -contains $run) { continue }
    $jobs = Invoke-Gh @('run', 'view', $run, '-R', $repo, '--json', 'jobs', '--jq', '.jobs[] | "\(.name)|\(.status)"')
    if ($jobs -notcontains "$stage|in_progress") { continue }
    $newRuns += $run
    $releases += $title.Split(' ')[-1]
  }
  if ($newRuns.Count -eq 0) { exit 0 }

  Add-Content -Path $asked -Value $newRuns
  Write-Log "== $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') waiting for the PC: $($releases -join ' ')"

  Add-Type -AssemblyName System.Windows.Forms, System.Drawing
  [Windows.Forms.Application]::EnableVisualStyles()
  $form = New-Object Windows.Forms.Form
  $form.Text = 'AAX signing'
  $form.FormBorderStyle = 'FixedDialog'
  $form.StartPosition = 'CenterScreen'
  $form.MaximizeBox = $false
  $form.MinimizeBox = $false
  $form.TopMost = $true
  $form.AutoSize = $true
  $form.AutoSizeMode = 'GrowAndShrink'
  $form.Padding = New-Object Windows.Forms.Padding(16)
  $label = New-Object Windows.Forms.Label
  $label.AutoSize = $true
  $label.MaximumSize = New-Object Drawing.Size(420, 0)
  $label.Text = "Ready to sign the AAX for:`r`n`r`n" + ($releases -join "`r`n") + "`r`n`r`nMove the iLok to this PC, then click Sign."
  $label.Location = New-Object Drawing.Point(16, 16)
  $form.Controls.Add($label)
  $top = $label.PreferredHeight + 32
  $sign = New-Object Windows.Forms.Button
  $sign.Text = 'Sign'
  $sign.DialogResult = 'OK'
  $sign.Location = New-Object Drawing.Point(250, $top)
  $later = New-Object Windows.Forms.Button
  $later.Text = 'Later'
  $later.DialogResult = 'Cancel'
  $later.Location = New-Object Drawing.Point(160, $top)
  $form.Controls.AddRange(@($later, $sign))
  $form.AcceptButton = $sign
  $form.CancelButton = $later
  # Gives up after 20 minutes (counts as Later), as the Mac's prompt does.
  $timer = New-Object Windows.Forms.Timer
  $timer.Interval = 20 * 60 * 1000
  $timer.Add_Tick({ $timer.Stop(); $form.DialogResult = 'Cancel'; $form.Close() })
  $form.Add_Shown({ $timer.Start(); $form.Activate() })
  $answer = $form.ShowDialog()
  $timer.Dispose()
  $form.Dispose()

  if ($answer -ne 'OK') {
    Write-Log '   later (run sign-aax when the iLok is in this PC)'
    exit 0
  }

  Show-Toast 'AAX signing' ("Signing " + ($releases -join ', '))
  # sign-aax.ps1 writes its own output to the log.
  $output = @(& $signAax *>&1 | ForEach-Object { "$_" })
  $status = $LASTEXITCODE
  if ($status -ne 0) {
    $reason = $output | Where-Object { $_ -match 'wraptool Error|LICENSE ERROR|iLok|failed|not found' } | Select-Object -First 1
    if (-not $reason) { $reason = "See $log" }
    if ($reason.Length -gt 180) { $reason = $reason.Substring(0, 180) }
    Show-Toast 'AAX signing failed' "$reason. Fix it, then run sign-aax."
    exit 1
  }
  $done = ($output | Where-Object { $_ -like 'Signed:*' } | ForEach-Object { ($_ -replace '^Signed:\s*', '') }) -join ', '
  if (-not $done) { $done = 'Nothing needed signing' }
  Show-Toast 'Signed on the PC' "$done."
  exit 0
} finally {
  $mutex.ReleaseMutex()
  $mutex.Dispose()
}
