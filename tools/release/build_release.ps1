# Builds a TombStone Engine release for Windows x64:
#   tools\release\build_release.ps1 [-Out <dir>] [-BuildDir build] [-SkipBuild] [-SkipSmoke] [-TestInstall]
# 1. Release build (VS 2022 x64), 2. ts_admin --smoke (TS_AUDIO=null),
# 3. stages the engine (ts_admin, ts_game, ts_server, ts_client, C++ runtime
#    DLLs, plugins, templates, samples, fonts, docs, script SDK, licences),
# 4. writes TombStone-<VERSION>-win64.zip, TombStone-<VERSION>-setup.exe (the
#    full installer) and TombStone-<VERSION>-update.exe (same AppId, upgrades
#    an install in place with a minimal wizard; the editor's update notifier
#    prefers it), both from tools\release\tombstone.iss, plus SHA256SUMS.txt.
# -TestInstall then installs the setup silently per-user into a temp folder,
# runs the installed ts_admin --version and --smoke, runs the update over
# it, and uninstalls it.
# The version comes from the VERSION file at the repo root. Nothing is
# tagged or published.
param(
  [string]$Out = "",
  [string]$BuildDir = "build",
  [switch]$SkipBuild,
  [switch]$SkipSmoke,
  [switch]$TestInstall
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$version = (Get-Content (Join-Path $root "VERSION") -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must be MAJOR.MINOR.PATCH (got '$version')" }
if (-not $Out) { $Out = Join-Path $root "release\$version" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
"TombStone Engine $version -> $Out"

function Step($name) { "`n== $name ==" }
# Native tools write progress to stderr; under "Stop" Windows PowerShell 5
# would turn that into an error, so run them with "Continue" and check
# $LASTEXITCODE instead.
function Native([scriptblock]$b) {
  $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
  try { & $b } finally { $ErrorActionPreference = $eap }
}
function Find-Iscc {
  $c = @()
  if ($env:TS_ISCC) { $c += $env:TS_ISCC }
  $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
  if ($cmd) { $c += $cmd.Source }
  $c += "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
  foreach ($p in $c) { if ($p -and (Test-Path $p)) { return $p } }
  return $null
}

# --- 1. Build -----------------------------------------------------------------
if (-not $SkipBuild) {
  Step "Release build ($build)"
  if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    Native { cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64 }
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
  }
  Native { cmake --build $build --config Release -- /m /v:m }
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
$bin = @{
  admin  = Join-Path $build "apps\admin\Release\ts_admin.exe"
  game   = Join-Path $build "apps\game\Release\ts_game.exe"
  server = Join-Path $build "apps\server\Release\ts_server.exe"
  client = Join-Path $build "apps\client\Release\ts_client.exe"
}
foreach ($k in $bin.Keys) { if (-not (Test-Path $bin[$k])) { throw "missing $($bin[$k]); build Release first" } }
$built = (Native { & $bin.admin --version } | Out-String).Trim()
if ($built -ne "TombStone Engine $version") { throw "ts_admin reports '$built', VERSION says $version (re-run CMake)" }

# --- 2. Smoke -----------------------------------------------------------------
$env:TS_AUDIO = "null"
if (-not $SkipSmoke) {
  Step "ts_admin --smoke"
  $log = Join-Path $Out "smoke.log"
  Push-Location (Split-Path $bin.admin)
  try { Native { & $bin.admin --smoke *> $log }; $rc = $LASTEXITCODE } finally { Pop-Location }
  Get-Content $log -Tail 3
  if ($rc -ne 0) { throw "ts_admin --smoke failed ($rc); see $log" }
}

# --- 3. Stage -----------------------------------------------------------------
Step "Stage"
$name = "TombStone-$version-win64"
$stageRoot = Join-Path $Out "stage"
$stage = Join-Path $stageRoot $name
if (Test-Path $stageRoot) { Remove-Item -Recurse -Force $stageRoot }   # our own previous output
New-Item -ItemType Directory -Force $stage | Out-Null
function CopyDir($from, $to) {
  if (-not (Test-Path $from)) { throw "missing $from" }
  New-Item -ItemType Directory -Force $to | Out-Null
  Copy-Item -Recurse -Force (Join-Path $from "*") $to
}
foreach ($k in $bin.Keys) {
  Copy-Item $bin[$k] $stage
  # App-local C++ runtime (vcruntime140*, msvcp140*, concrt140) and any other
  # DLL the build put beside an exe.
  Get-ChildItem (Split-Path $bin[$k]) -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $stage -Force }
}
foreach ($dll in "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll") {
  if (-not (Test-Path (Join-Path $stage $dll))) { throw "C++ runtime $dll was not staged (ts_game build copies it)" }
}
CopyDir (Join-Path $root "templates") (Join-Path $stage "templates")
New-Item -ItemType Directory -Force (Join-Path $stage "plugins") | Out-Null
Get-ChildItem (Join-Path $build "plugins") -Filter *.dll -ErrorAction SilentlyContinue |
  ForEach-Object { Copy-Item $_.FullName (Join-Path $stage "plugins") }
# Samples: every engine template's project, ready to open.
foreach ($t in Get-ChildItem (Join-Path $root "templates") -Directory) {
  $p = Join-Path $t.FullName "project"
  if (Test-Path $p) { CopyDir $p (Join-Path $stage "samples\$($t.Name)") }
}
# Fonts (embedded in the apps too) with their SIL OFL licences.
$fonts = Join-Path $stage "fonts"
New-Item -ItemType Directory -Force $fonts | Out-Null
Get-ChildItem (Join-Path $build "_deps") -Recurse -Filter *.ttf -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -match 'ts_font_' } | ForEach-Object { Copy-Item $_.FullName $fonts -Force }
Copy-Item (Join-Path $root "assets\fonts\*.txt") $fonts
if (-not (Get-ChildItem $fonts -Filter *.ttf)) { throw "fonts not found under $build\_deps" }
CopyDir (Join-Path $root "docs") (Join-Path $stage "docs")
Copy-Item (Join-Path $root "README.md") (Join-Path $stage "docs\README-engine.md")
CopyDir (Join-Path $root "sdk") (Join-Path $stage "sdk")
foreach ($f in "LICENSE", "README.md", "CHANGELOG.md", "VERSION") { Copy-Item (Join-Path $root $f) $stage }
# Third-party licences of everything linked in (FetchContent sources).
$lic = Join-Path $stage "licenses"
New-Item -ItemType Directory -Force $lic | Out-Null
foreach ($d in Get-ChildItem (Join-Path $build "_deps") -Directory -Filter "*-src") {
  $files = Get-ChildItem $d.FullName -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE)' }
  if ($files) {
    $to = Join-Path $lic ($d.Name -replace '-src$', '')
    New-Item -ItemType Directory -Force $to | Out-Null
    $files | ForEach-Object { Copy-Item $_.FullName $to }
  }
}
$count = (Get-ChildItem $stage -Recurse -File).Count
"staged $count files in $stage"

# --- 4. Zip + installer -------------------------------------------------------
Step "Zip"
$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
"zip: $zip"

Step "Installer"
Native { & (Join-Path $root "tools\installer\install_inno.ps1") | Out-Host }
$iscc = Find-Iscc
if (-not $iscc) { throw "ISCC.exe not found (tools\installer\install_inno.ps1 or TS_ISCC)" }
$setup = Join-Path $Out "TombStone-$version-setup.exe"
if (Test-Path $setup) { Remove-Item -Force $setup }
Native { & $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/DOutDir=$Out" (Join-Path $PSScriptRoot "tombstone.iss") }
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $setup)) { throw "ISCC failed ($LASTEXITCODE)" }
"setup: $setup"
$update = Join-Path $Out "TombStone-$version-update.exe"
if (Test-Path $update) { Remove-Item -Force $update }
Native { & $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/DOutDir=$Out" "/DUpdate=1" (Join-Path $PSScriptRoot "tombstone.iss") }
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $update)) { throw "ISCC (update) failed ($LASTEXITCODE)" }
"update: $update"

$sums = foreach ($f in $setup, $update, $zip) {
  "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $f).Hash.ToLower(), (Split-Path $f -Leaf)
}
$sums | Set-Content -Encoding ascii (Join-Path $Out "SHA256SUMS.txt")
$sums
foreach ($f in $setup, $update, $zip) { "{0}  {1:N0} bytes" -f (Split-Path $f -Leaf), (Get-Item $f).Length }

# --- 5. Optional: silent install test ----------------------------------------
if ($TestInstall) {
  Step "Silent install test"
  $t = Join-Path $env:TEMP "tombstone_release_test"
  if (Test-Path $t) { Remove-Item -Recurse -Force $t }
  $app = Join-Path $t "app"
  $work = Join-Path $t "work"
  New-Item -ItemType Directory -Force $work | Out-Null
  $p = Start-Process $setup -Wait -PassThru -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
    "/CURRENTUSER", "/DIR=`"$app`"", "/MERGETASKS=`"desktopicon`"", "/LOG=`"$t\install.log`""
  if ($p.ExitCode -ne 0) { throw "silent install exit code $($p.ExitCode)" }
  $menu = Join-Path ([Environment]::GetFolderPath("Programs")) "TombStone Engine"
  $desk = Join-Path ([Environment]::GetFolderPath("Desktop")) "TombStone Editor.lnk"
  foreach ($f in "$app\ts_admin.exe", "$app\ts_game.exe", "$app\ts_server.exe", "$app\ts_client.exe",
                 "$app\vcruntime140.dll", "$app\templates", "$app\samples", "$app\plugins", "$app\fonts",
                 "$app\docs\README.md", "$app\sdk\include\tombstone\script_sdk.h", "$app\unins000.exe",
                 "$menu\TombStone Editor.lnk", $desk) {
    if (-not (Test-Path $f)) { throw "after install, missing $f" }
  }
  $gpu = Select-String -Path "$t\install.log" -Pattern "GPU check" | Select -Last 1
  "installed to $app; shortcuts ok; $gpu"
  $reg = Get-ChildItem HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall |
    Where-Object { $_.PSChildName -like "*6F1D3B0A-7C52-4E1B-9A3D-5B2C8E4F7A10*" }
  if (-not $reg) { throw "no Apps & features entry (HKCU uninstall key)" }
  Push-Location $work
  try {
    $v = (Native { & "$app\ts_admin.exe" --version } | Out-String).Trim(); "installed ts_admin --version: $v"
    if ($v -ne "TombStone Engine $version") { throw "installed version '$v'" }
    Native { & "$app\ts_admin.exe" --smoke *> "$t\installed_smoke.log" }; $rc = $LASTEXITCODE
  } finally { Pop-Location }
  Get-Content "$t\installed_smoke.log" -Tail 2
  "installed ts_admin --smoke exit $rc"
  # The update over the install: same folder, still one Apps & features entry.
  $up = Start-Process $update -Wait -PassThru -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
    "/CURRENTUSER", "/LOG=`"$t\update.log`""
  if ($up.ExitCode -ne 0) { throw "silent update exit code $($up.ExitCode)" }
  if (-not (Test-Path "$app\ts_admin.exe")) { throw "after update, missing $app\ts_admin.exe" }
  $entries = @(Get-ChildItem HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall |
    Where-Object { $_.PSChildName -like "*6F1D3B0A-7C52-4E1B-9A3D-5B2C8E4F7A10*" })
  if ($entries.Count -ne 1) { throw "after update, $($entries.Count) Apps & features entries (want 1)" }
  "update over the install: same folder, one Apps & features entry"
  Copy-Item "$t\update.log" (Join-Path $Out "update.log") -Force
  Copy-Item "$t\installed_smoke.log" (Join-Path $Out "installed_smoke.log") -Force
  Copy-Item "$t\install.log" (Join-Path $Out "install.log") -Force
  $u = Start-Process "$app\unins000.exe" -Wait -PassThru -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"
  # The uninstaller hands over to a temp copy; wait for it to finish.
  for ($i = 0; $i -lt 120 -and (Test-Path "$app\ts_admin.exe"); $i++) { Start-Sleep 1 }
  Start-Sleep 2
  foreach ($f in "$app\ts_admin.exe", "$app\templates", "$menu\TombStone Editor.lnk", $desk) {
    if (Test-Path $f) { throw "after uninstall, still there: $f" }
  }
  $reg = Get-ChildItem HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall |
    Where-Object { $_.PSChildName -like "*6F1D3B0A-7C52-4E1B-9A3D-5B2C8E4F7A10*" }
  if ($reg) { throw "uninstall left the Apps & features entry" }
  "uninstalled: files, shortcuts and the Apps & features entry are gone"
  if ($rc -ne 0) { throw "installed ts_admin --smoke failed ($rc)" }
  "INSTALL TEST OK"
}
"RELEASE OK $version"
