# Build the installable mod folder and the release archive.
#
# THE RULE THIS SCRIPT EXISTS TO ENFORCE: it copies a NAMED LIST of files out of dist/ and config/.
# It never copies a directory tree, and it never copies the repository root. `Thuum/` is a pristine
# upstream copy of Nexus 50559 held here as read-only reference (AGENTS.md) and must not reach a
# release; with an allow-list there is no path by which it can, rather than a rule someone has to
# remember. Same for extern/, build/, .scratch/ and the 21MB .pdb.
#
# Deployment into an MO2 mods folder is a separate, opt-in switch. AGENTS.md requires shipping as an
# MO2 mod folder and never into STOCK GAME\Data, and -Deploy is previewed before it writes.
#
#   .\tools\package.ps1                        # stage + archive into release/
#   .\tools\package.ps1 -Deploy                # also install into the MO2 mods folder
#   .\tools\package.ps1 -Deploy -Force         # skip the confirmation prompt
#
[CmdletBinding()]
param(
    # Install the staged folder into the MO2 mods directory as well. Off by default: writing into
    # a live MO2 instance is a mutation of a shared resource, not a side effect of packaging.
    [switch]$Deploy,
    [switch]$Force,
    [string]$ModsRoot = 'C:\Nolvus\Instances\Nolvus Awakening\MODS\mods'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ModName  = 'Shouts for MCO'

# Read the version from CMakeLists rather than carrying a second copy of it. Two places to edit is
# how a folder name ends up disagreeing with the DLL's version resource.
$cmake = Get-Content (Join-Path $RepoRoot 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$') {
    throw "Could not read VERSION from CMakeLists.txt -- refusing to guess it."
}
$Version = $Matches[1]

Write-Host "$ModName $Version"

# THE ALLOW-LIST. Source path -> path inside the mod folder. Nothing else ships.
$Payload = @(
    @{ From = 'dist\SKSE\Plugins\ShoutMCO.dll'; To = 'SKSE\Plugins\ShoutMCO.dll'; Required = $true }
    # Shipped from config/, not from dist/. dist/ holds whatever the last live session left behind
    # -- these settings are re-read per shout precisely so they can be edited in place mid-run
    # (finding 8) -- so staging from there would ship somebody's debugging state as the defaults.
    @{ From = 'config\ShoutMCO.ini';            To = 'SKSE\Plugins\ShoutMCO.ini'; Required = $true }
    @{ From = 'docs\release\README.txt';        To = 'README.txt';                Required = $true }
    @{ From = 'LICENSE';                        To = 'LICENSE';                   Required = $true }
)
# ShoutMCO.pdb is deliberately absent: 21MB of symbols against a 616KB DLL. Anyone diagnosing a
# crash log can be handed the matching .pdb from dist/ on request.

$StageRoot = Join-Path $RepoRoot 'release'
$StageDir  = Join-Path $StageRoot "$ModName $Version"

if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
New-Item -ItemType Directory -Path $StageDir -Force | Out-Null

foreach ($item in $Payload) {
    $src = Join-Path $RepoRoot $item.From
    if (-not (Test-Path $src)) {
        if ($item.Required) { throw "Missing required payload file: $($item.From). Build first (cmake --build --preset ALL-Release)." }
        continue
    }
    $dst = Join-Path $StageDir $item.To
    New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
    Copy-Item $src $dst -Force
    Write-Host ("  staged {0,-28} -> {1}" -f $item.From, $item.To)
}

# Verify the DLL's own version resource agrees with the folder name. The acceptance criterion is
# consistency between CMakeLists, the plugin declaration and the folder, and only the binary can
# report the middle one -- reading CMakeLists twice would prove nothing.
$dll = Join-Path $StageDir 'SKSE\Plugins\ShoutMCO.dll'
$fileVersion = (Get-Item $dll).VersionInfo.FileVersion
Write-Host "  DLL version resource: $fileVersion"
# EXACT, not a prefix match. `-like "$Version*"` accepted 1.0.0.1 -- and 1.0.01 -- while CMakeLists
# said 1.0.0, so the guard would have waved through the very mismatch it exists to catch.
# cmake/version.rc.in always emits four components as "@PROJECT_VERSION@.0", so that is the exact
# string to expect.
$expectedVersion = "$Version.0"
if ($fileVersion -ne $expectedVersion) {
    throw "DLL reports '$fileVersion' but CMakeLists says '$Version' (expected '$expectedVersion'). Rebuild before packaging."
}

$Archive = Join-Path $StageRoot "$($ModName -replace ' ','-')-$Version.zip"
if (Test-Path $Archive) { Remove-Item $Archive -Force }
Compress-Archive -Path (Join-Path $StageDir '*') -DestinationPath $Archive

Write-Host "`nArchive: $Archive"
Write-Host 'Contents:'
$entries = [System.IO.Compression.ZipFile]::OpenRead($Archive)
try {
    $entries.Entries | ForEach-Object { Write-Host ("  {0,-34} {1,9} bytes" -f $_.FullName, $_.Length) }
    # Belt and braces over the allow-list: assert the outcome, not the intent. Ticket 16 asks for
    # this checked by listing the archive rather than by trusting the packaging step.
    $forbidden = $entries.Entries | Where-Object { $_.FullName -match '(?i)(^|/)(Thuum|extern|build|\.scratch)/' -or $_.FullName -match '(?i)\.pdb$' }
    if ($forbidden) { throw "Archive contains files that must never ship: $($forbidden.FullName -join ', ')" }
} finally {
    $entries.Dispose()
}
Write-Host 'Verified: no Thuum/, no extern/, no build/, no .scratch/, no .pdb.'

if (-not $Deploy) {
    Write-Host "`nNot deployed. Re-run with -Deploy to install into the MO2 mods folder."
    return
}

# AGENTS.md: never write into STOCK GAME\Data. -ModsRoot is a parameter, so that invariant has to be
# enforced here rather than assumed -- and this branch goes on to Remove-Item -Recurse the target,
# which under a game Data directory would be destructive rather than merely wrong.
# Checked on the RESOLVED path so ..\ or a junction cannot walk into it behind the string compare.
$resolvedRoot = try { (Resolve-Path -LiteralPath $ModsRoot -ErrorAction Stop).Path } catch { $ModsRoot }
if ($resolvedRoot -match '(?i)[\\/]STOCK GAME[\\/]' -or $resolvedRoot -match '(?i)[\\/]Data[\\/]?$') {
    throw "Refusing to deploy into '$resolvedRoot'. AGENTS.md forbids writing to STOCK GAME\Data; ship to an MO2 mods folder."
}

$Target = Join-Path $ModsRoot $ModName
Write-Host "`nDeploy preview:"
Write-Host "  from $StageDir"
Write-Host "  to   $Target"
if (Test-Path $Target) {
    Write-Host "  (replacing an existing folder -- its current contents will be removed)"
    Get-ChildItem $Target -Recurse -File | ForEach-Object { Write-Host "    exists: $($_.FullName.Substring($Target.Length + 1))" }
} else {
    Write-Host '  (new folder)'
}

if (-not $Force) {
    $answer = Read-Host 'Proceed? (y/N)'
    if ($answer -ne 'y') { Write-Host 'Aborted.'; return }
}

if (Test-Path $Target) { Remove-Item $Target -Recurse -Force }
New-Item -ItemType Directory -Path $Target -Force | Out-Null
Copy-Item (Join-Path $StageDir '*') $Target -Recurse -Force

# MO2 lists a folder without meta.ini, but shows it as having no origin and no version. Writing one
# is what makes the deployed copy identifiable as this release rather than a stray folder.
@"
[General]
modid=0
version=d$Version
newestVersion=
category=0
installationFile=
comments=$ModName $Version -- packaged by tools/package.ps1
"@ | Set-Content (Join-Path $Target 'meta.ini') -Encoding UTF8

Write-Host "`nDeployed. Enable '$ModName' in MO2 and place it after ADXP - MCO."
Get-ChildItem $Target -Recurse -File | ForEach-Object { Write-Host "  $($_.FullName.Substring($Target.Length + 1))" }
