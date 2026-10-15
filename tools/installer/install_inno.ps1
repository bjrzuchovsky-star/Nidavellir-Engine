# Installs Inno Setup 6 (ISCC.exe) for the Freight Office installer export
# when it is missing. Per-user (no admin): winget first, then the official
# jrsoftware.org installer. Prints the ISCC.exe path.
$ErrorActionPreference = "Stop"
function Find-Iscc {
  $c = @()
  if ($env:TS_ISCC) { $c += $env:TS_ISCC }
  $c += "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
  $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
  if ($cmd) { $c += $cmd.Source }
  foreach ($p in $c) { if ($p -and (Test-Path $p)) { return $p } }
  return $null
}
$iscc = Find-Iscc
if ($iscc) { "Inno Setup already installed: $iscc"; exit 0 }
if (Get-Command winget -ErrorAction SilentlyContinue) {
  "Installing Inno Setup 6 with winget (per-user)..."
  winget install --id JRSoftware.InnoSetup -e --scope user --silent --accept-package-agreements --accept-source-agreements | Out-Host
  $iscc = Find-Iscc
}
if (-not $iscc) {
  "Downloading Inno Setup 6 from jrsoftware.org..."
  $tmp = Join-Path $env:TEMP "innosetup-setup.exe"
  Invoke-WebRequest -UseBasicParsing "https://jrsoftware.org/download.php/is.exe" -OutFile $tmp
  $sig = Get-AuthenticodeSignature $tmp
  if ($sig.Status -ne "Valid") { throw "Inno Setup download is not validly signed ($($sig.Status))" }
  $p = Start-Process $tmp -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/CURRENTUSER" -Wait -PassThru
  if ($p.ExitCode -ne 0) { throw "Inno Setup installer exit code $($p.ExitCode)" }
  $iscc = Find-Iscc
}
if (-not $iscc) { throw "Inno Setup install finished but ISCC.exe was not found" }
"Inno Setup ready: $iscc"
