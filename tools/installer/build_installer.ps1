# Exports a project and builds its Windows setup installer + portable zip.
#   tools\installer\build_installer.ps1 -Project <dir|sample:ghost-town-3d> -Out D:\Dev\TombStoneExports
#     [-Admin build\apps\admin\Release\ts_admin.exe] [-Publisher "..."] [-License EULA.txt]
#     [-InstallMode ask|user|all] [-Version 1.0.0]
# Installs Inno Setup first when it is missing (install_inno.ps1).
param(
  [Parameter(Mandatory = $true)][string]$Project,
  [Parameter(Mandatory = $true)][string]$Out,
  [string]$Admin = "",
  [string]$Publisher = "",
  [string]$License = "",
  [string]$InstallMode = "",
  [string]$Version = ""
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
& (Join-Path $PSScriptRoot "install_inno.ps1") | Out-Host
if (-not $Admin) {
  foreach ($c in "build\apps\admin\Release\ts_admin.exe", "build\apps\admin\ts_admin.exe") {
    $p = Join-Path $root $c
    if (Test-Path $p) { $Admin = $p; break }
  }
}
if (-not $Admin -or -not (Test-Path $Admin)) { throw "ts_admin.exe not found; build Release first" }
$exportArgs = @("--export", $Project, "--out", $Out, "--zip", "--installer")
if ($Publisher) { $exportArgs += "--publisher", $Publisher }
if ($License) { $exportArgs += "--license", $License }
if ($InstallMode) { $exportArgs += "--install-mode", $InstallMode }
if ($Version) { $exportArgs += "--version", $Version }
& $Admin @exportArgs
exit $LASTEXITCODE
