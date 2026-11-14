# Signs plug-ins' Windows AAX with PACE's wraptool on this PC and attaches them to their releases. Pro Tools
# only loads PACE-signed AAX plug-ins, and PACE signing needs the iLok, which moves by hand between the Mac and
# this PC, so this runs here when you start it (the `sign-aax` command, or the Sign button in the prompt that
# bundle/install-aax-signer-windows.ps1 sets up). It is the PC's twin of bundle/sign-aax.sh.
#
#   sign-aax                    sign every release whose "AAX signing" run is waiting at "Sign on the PC"
#   sign-aax reversinator ...   sign these plug-ins' newest releases, waiting or not
#   sign-aax -Force ...         re-sign even if the release already has a signed Windows AAX
#   sign-aax -Local ...         sign and verify only: no upload (leaves the zips in .\aax-signed)
#
# For each release it takes the unsigned AAX its build uploaded as an artifact ("<Name>-Windows-AAX-unsigned"),
# signs it, checks the signature and uploads "<Name>-<version>-Windows-AAX.zip" to the release. There's no
# notarisation on Windows, so the release's "AAX signing" run finishes as soon as the zip is there.
#
# Needs: gh (logged in), PACE's AAX code signing tools, the iLok, a code-signing certificate in
# Cert:\CurrentUser\My whose key is in a legacy CryptoAPI provider (wraptool can't use CNG keys), and
# %APPDATA%\SammyJ\pace.json:
#   { "account": "<iLok user ID>", "customerNumber": "<from PACE Central>",
#     "customerName": "<company name as PACE Central has it>", "signId": "<certificate SHA1 thumbprint>",
#     "wraptool": "C:\\Program Files\\PACEAntiPiracy\\Eden\\Fusion\\Versions\\6\\bin\\wraptool.exe" }
# The first signing asks wraptool for your iLok password once (run wraptool with --password yourself); it
# keeps it after that. Works in Windows PowerShell 5.1.

$ErrorActionPreference = 'Stop'
$repo = if ($env:GH_REPO) { $env:GH_REPO } else { 'samueljustice/plugins' }
$config = if ($env:SAMMYJ_PACE_CONFIG) { $env:SAMMYJ_PACE_CONFIG } else { Join-Path $env:APPDATA 'SammyJ\pace.json' }
$stage = 'Sign on the PC'
$here = $PSScriptRoot

$force = $false
$localOnly = $false
$wanted = @()
foreach ($arg in $args) {
  switch -Regex ([string]$arg) {
    '^--?force$' { $force = $true; continue }
    '^--?local$' { $localOnly = $true; continue }
    '^(-h|--?help|-\?)$' {
      Get-Content $PSCommandPath -TotalCount 22 | ForEach-Object { $_ -replace '^# ?', '' }
      exit 0
    }
    '^-' { Write-Host "Unknown option $arg"; exit 2 }
    default { $wanted += [string]$arg }
  }
}

# Runs a program and returns its exit code and output. Builds the command line itself, because Windows
# PowerShell 5.1 drops quotes inside arguments it passes to programs, and reads the output as UTF-8.
function Invoke-Tool {
  param([string]$Exe, [string[]]$ArgList, [int]$TimeoutMinutes = 15)
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
  $psi.FileName = $Exe
  $psi.Arguments = $quoted -join ' '
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
  $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
  $p = [Diagnostics.Process]::Start($psi)
  $out = $p.StandardOutput.ReadToEndAsync()
  $err = $p.StandardError.ReadToEndAsync()
  if (-not $p.WaitForExit($TimeoutMinutes * 60000)) {
    try { $p.Kill() } catch {}
    return [pscustomobject]@{ ExitCode = -1; Out = $out.Result; Err = "Timed out after $TimeoutMinutes minutes" }
  }
  $p.WaitForExit()
  [pscustomobject]@{ ExitCode = $p.ExitCode; Out = $out.Result; Err = $err.Result }
}

function Invoke-Gh {
  param([string[]]$ArgList, [switch]$AllowFailure)
  $r = Invoke-Tool -Exe 'gh' -ArgList $ArgList
  if ($r.ExitCode -ne 0 -and -not $AllowFailure) {
    throw "gh $($ArgList -join ' ') failed: $($r.Err.Trim())"
  }
  $r
}

function Get-Lines([string]$Text) { @($Text -split "`r?`n" | Where-Object { $_ -ne '' }) }

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { Write-Host 'gh (GitHub CLI) not found'; exit 1 }
if (-not (Test-Path $config)) { Write-Host "Missing $config (see the top of this script)"; exit 1 }
$pace = Get-Content $config -Raw | ConvertFrom-Json
foreach ($k in 'account', 'customerNumber', 'customerName', 'signId') {
  if (-not $pace.$k) { Write-Host "$config has no ""$k"""; exit 1 }
}
$wraptool = if ($pace.wraptool) { $pace.wraptool } else { 'C:\Program Files\PACEAntiPiracy\Eden\Fusion\Versions\6\bin\wraptool.exe' }
if (-not (Test-Path $wraptool)) { Write-Host "wraptool not found at $wraptool"; exit 1 }
# wraptool needs PACE_FUSION_HOME, which shells opened before PACE's installer ran don't have.
if (-not $env:PACE_FUSION_HOME) {
  $env:PACE_FUSION_HOME = [Environment]::GetEnvironmentVariable('PACE_FUSION_HOME', 'Machine')
  if (-not $env:PACE_FUSION_HOME) { $env:PACE_FUSION_HOME = (Split-Path (Split-Path $wraptool)) + '\' }
}
$cert = Get-Item "Cert:\CurrentUser\My\$($pace.signId)" -ErrorAction SilentlyContinue
if (-not $cert -or -not $cert.HasPrivateKey) {
  Write-Host "Signing certificate $($pace.signId) (with its private key) isn't in Cert:\CurrentUser\My"; exit 1
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("sammyj-aax-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
$out = Join-Path (Get-Location).Path 'aax-signed'
$plugins = @(Get-Content (Join-Path $here 'plugins.txt') | Where-Object { $_ -and -not $_.StartsWith('#') })

try {
  # What to sign: id, tag and version of each release.
  $targets = @()
  if ($wanted.Count -gt 0) {
    # Each plug-in's newest published release whose tag is "<prefix>X.Y.Z" (as bundle/resolve-releases.sh).
    $tags = Get-Lines (Invoke-Gh @('release', 'list', '-R', $repo, '--limit', '200', '--exclude-drafts',
                                   '--exclude-pre-releases', '--json', 'tagName', '--jq', '.[].tagName')).Out
    foreach ($id in $wanted) {
      $line = $plugins | Where-Object { $_.Split('|')[0] -eq $id } | Select-Object -First 1
      if (-not $line) { Write-Host "Unknown plug-in or no release: $id"; exit 1 }
      $prefix = $line.Split('|')[1]
      $tag = $tags | Where-Object { $_ -match ('^' + [regex]::Escape($prefix) + '\d+\.\d+\.\d+$') } |
               Sort-Object { [version]$_.Substring($prefix.Length) } | Select-Object -Last 1
      if (-not $tag) { Write-Host "Unknown plug-in or no release: $id"; exit 1 }
      $targets += [pscustomobject]@{ Id = $id; Tag = $tag; Version = $tag.Substring($prefix.Length) }
    }
  } else {
    # Releases whose "AAX signing" run is waiting at this machine's stage. Run titles end with the tag.
    $runs = Get-Lines (Invoke-Gh @('run', 'list', '-R', $repo, '--workflow', 'aax-signing.yml', '--status', 'in_progress',
                                   '--limit', '50', '--json', 'databaseId,displayTitle', '--jq',
                                   '.[] | "\(.databaseId)|\(.displayTitle)"')).Out
    foreach ($r in $runs) {
      $run, $title = $r.Split('|', 2)
      $jobs = Get-Lines (Invoke-Gh @('run', 'view', $run, '-R', $repo, '--json', 'jobs', '--jq',
                                     '.jobs[] | "\(.name)|\(.status)"')).Out
      if ($jobs -notcontains "$stage|in_progress") { continue }
      $tag = $title.Split(' ')[-1]
      $cut = $tag.LastIndexOf('-v')
      if ($cut -lt 0) { continue }
      $targets += [pscustomobject]@{ Id = $tag.Substring(0, $cut); Tag = $tag; Version = $tag.Substring($cut + 2) }
    }
    if ($targets.Count -eq 0) {
      Write-Host 'Nothing is waiting for the PC. To sign a release anyway: sign-aax <plug-in>, e.g. sign-aax reversinator'
      exit 0
    }
  }

  $signed = @()
  $skipped = @()

  foreach ($t in $targets) {
    $line = $plugins | Where-Object { $_.Split('|')[0] -eq $t.Id } | Select-Object -First 1
    if (-not $line) { Write-Host "== $($t.Tag): not in bundle/plugins.txt"; $skipped += "$($t.Tag): unknown plug-in"; continue }
    $fields = $line.Split('|')
    $name = $fields[2]
    $bundle = $fields[3]
    Write-Host "== $name $($t.Version) ($($t.Tag))"

    $assets = Get-Lines (Invoke-Gh @('release', 'view', $t.Tag, '-R', $repo, '--json', 'assets', '--jq', '.assets[].name')).Out
    if (-not $force -and -not $localOnly -and ($assets -match '-Windows-AAX\.zip$')) {
      Write-Host '   already has a signed Windows AAX (use -Force to redo it)'
      $skipped += "${name}: already signed"
      continue
    }

    $run = (Invoke-Gh @('run', 'list', '-R', $repo, '--workflow', "build-release_$($t.Id).yml", '--branch', $t.Tag,
                        '--status', 'success', '--limit', '1', '--json', 'databaseId', '--jq', '.[0].databaseId // empty')).Out.Trim()
    if (-not $run) {
      Write-Host "   no successful build of $($t.Tag) found"
      $skipped += "${name}: no build of $($t.Tag)"
      continue
    }
    $artifact = Get-Lines (Invoke-Gh @('api', "repos/$repo/actions/runs/$run/artifacts", '--jq',
                                       '.artifacts[] | select(.name | endswith("-Windows-AAX-unsigned")) | select(.expired | not) | .name')).Out |
                  Select-Object -First 1
    if (-not $artifact) {
      Write-Host "   the $($t.Tag) build has no (unexpired) Windows AAX artifact"
      $skipped += "${name}: no AAX artifact in $($t.Tag)'s build"
      continue
    }
    $short = $artifact.Substring(0, $artifact.Length - '-Windows-AAX-unsigned'.Length)

    $dir = Join-Path $work $t.Id
    New-Item -ItemType Directory -Force (Join-Path $dir 'dl') | Out-Null
    Invoke-Gh @('run', 'download', $run, '-R', $repo, '-n', $artifact, '-D', (Join-Path $dir 'dl')) | Out-Null
    $zip = Get-ChildItem (Join-Path $dir 'dl') -Filter *.zip | Select-Object -First 1
    if (-not $zip) { Write-Host "   $artifact has no zip in it"; exit 1 }
    Expand-Archive -Path $zip.FullName -DestinationPath $dir -Force
    $plugin = Join-Path $dir "$bundle.aaxplugin"
    $binary = Join-Path $plugin "Contents\x64\$bundle.aaxplugin"
    if (-not (Test-Path $binary)) { Write-Host "   $bundle.aaxplugin\Contents\x64\$bundle.aaxplugin not in $artifact"; exit 1 }

    # Sign the outer folder; wraptool finds and signs Contents\x64\<bundle>.aaxplugin itself.
    Write-Host "   signing with PACE (account $($pace.account))"
    $r = Invoke-Tool -Exe $wraptool -ArgList @('sign', '--allowsigningservice', '--account', $pace.account,
           '--customernumber', $pace.customerNumber, '--customername', $pace.customerName,
           '--productname', $bundle, '--signid', $pace.signId, '--in', $plugin)
    $log = ($r.Out + $r.Err).Trim()
    if ($r.ExitCode -ne 0) {
      Write-Host $log
      Write-Host '   wraptool failed. The iLok may not be in this PC: plug it in here, then run sign-aax again.'
      exit 1
    }

    # PACE's check (Windows only accepts the inner DLL), then that Windows sees our certificate's signature.
    # A self-signed certificate isn't trusted by Windows, so only the signer is checked, not the trust.
    $v = Invoke-Tool -Exe $wraptool -ArgList @('verify', '--in', $binary)
    $verify = ($v.Out + $v.Err).Trim()
    if ($verify -notmatch 'The binary was signed') { Write-Host $verify; Write-Host "   PACE signature didn't verify"; exit 1 }
    $auth = Get-AuthenticodeSignature $binary
    if (-not $auth.SignerCertificate -or $auth.SignerCertificate.Thumbprint -ne $pace.signId) {
      Write-Host "   the DLL isn't Authenticode-signed with certificate $($pace.signId) ($($auth.StatusMessage))"; exit 1
    }

    # tar.exe (not Compress-Archive) so hidden files such as desktop.ini are kept.
    $final = "$short-$($t.Version)-Windows-AAX.zip"
    $dest = if ($localOnly) { $out } else { $dir }
    New-Item -ItemType Directory -Force $dest | Out-Null
    $zipPath = Join-Path $dest $final
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    $z = Invoke-Tool -Exe 'tar.exe' -ArgList @('-a', '-c', '-f', $zipPath, '-C', $dir, "$bundle.aaxplugin")
    if ($z.ExitCode -ne 0) { Write-Host $z.Err; Write-Host "   couldn't zip $bundle.aaxplugin"; exit 1 }

    if ($localOnly) {
      Write-Host "   signed: $zipPath (not uploaded)"
      $signed += "$name $($t.Version) (local only)"
      continue
    }
    Invoke-Gh @('release', 'upload', $t.Tag, '-R', $repo, $zipPath, '--clobber') | Out-Null
    Write-Host "   uploaded $final"
    $signed += "$name $($t.Version)"
  }

  Write-Host ''
  foreach ($s in $signed) { Write-Host "Signed:  $s" }
  foreach ($s in $skipped) { Write-Host "Skipped: $s" }
  if (-not $localOnly -and $signed.Count -gt 0) {
    Write-Host "Published; each release's AAX signing run finishes once it sees the Windows AAX."
  }
  exit 0
} finally {
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
