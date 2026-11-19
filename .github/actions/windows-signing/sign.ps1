# Signs Windows files with Azure Artifact Signing, then checks each signature.
#   sign.ps1 -Description "Swooshbuckler" path\to\file.exe path\to\plugin.vst3 ...
# Needs the windows-signing action to have run first (it sets SAMMYJ_SIGNTOOL, SAMMYJ_SIGN_DLIB, SAMMYJ_SIGN_METADATA).
param(
  [Parameter(Mandatory)] [string] $Description,
  [Parameter(Mandatory, ValueFromRemainingArguments)] [string[]] $Files
)
$ErrorActionPreference = 'Stop'
if (-not $env:SAMMYJ_SIGN_DLIB) { throw "SAMMYJ_SIGN_DLIB isn't set: run the windows-signing action first" }

$paths = foreach ($f in $Files) { (Resolve-Path -LiteralPath $f).Path }
# One SignTool call for the lot (each file is one signature on the account's monthly allowance).
Write-Host "Signing $($paths.Count) file(s): $($paths -join ', ')"
& $env:SAMMYJ_SIGNTOOL sign /v /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 `
  /dlib $env:SAMMYJ_SIGN_DLIB /dmdf $env:SAMMYJ_SIGN_METADATA /d $Description @paths
if ($LASTEXITCODE -ne 0) { throw "SignTool failed with exit code $LASTEXITCODE" }

foreach ($p in $paths) {
  & $env:SAMMYJ_SIGNTOOL verify /pa /q $p
  if ($LASTEXITCODE -ne 0) { throw "Signature check failed for $p" }
  $sig = Get-AuthenticodeSignature -LiteralPath $p
  Write-Host "Signed: $p ($($sig.SignerCertificate.Subject))"
}
