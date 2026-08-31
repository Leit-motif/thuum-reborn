# Build the installable mod folder and the release archive.
#
# THE RULE THIS SCRIPT EXISTS TO ENFORCE: it copies a NAMED LIST of files out of dist/ and config/.
# It never copies the repository root. `Thuum/` is a pristine upstream copy of Nexus 50559 held here
# as read-only reference (AGENTS.md) and must not reach a release; with an allow-list there is no
# path by which it can, rather than a rule someone has to remember. Same for extern/, build/,
# .scratch/ and the 21MB .pdb.
#
# ONE DIRECTORY IS COPIED WHOLE, and it is fenced rather than trusted. The Nemesis patch is an
# open-ended set of per-node files -- later work adds more, and a hand-maintained
# list of `#0094.txt`-style names is a list someone forgets to update, which ships a patch that
# generates a half-edited graph. So `nemesis\` is staged as a tree, and every file in it must sit
# at a legal Nemesis patch PATH -- see `Test-NemesisPatchPath`, which checks the whole relative
# path rather than the filename. The allow-list property survives: an unexpected file there is a
# loud error, not a passenger.
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
    # Package a tree with uncommitted changes anyway. Off by default: an artifact from a dirty
    # tree cannot be attributed to any commit, which is how the 1.0.0 zip was once silently
    # replaced by a mid-development build. The stamp then carries
    # "+dirty" so the artifact itself confesses.
    [switch]$AllowDirty,
    # Skip the animation-pack verifier. Present so a diagnosis run is possible, not so a release
    # can skip it.
    [switch]$SkipPackVerify,
    [string]$ModsRoot = 'C:\Nolvus\Instances\Nolvus Awakening\MODS\mods'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
# The PRODUCT name, owner ruling 2026-08-28. Internal `ShoutMCO` names -- DLL, INI, log filename,
# every archived trace path -- are deliberately unchanged; only what the player sees carries this.
$ModName  = "Thu'um - Fully Animated Shouts Reborn"

# THE ANIMATIONS SHIP INSIDE THIS ZIP. Owner ruling 2026-08-28: "the animations are not a separate
# file. they will be shipped with the mod." A previous build produced two archives from two scripts;
# that split was an agent inference off AGENTS.md's architectural-independence line, which is about
# COUPLING and not about packaging. tools/package-pack.ps1 is gone and its fence lives here.
$PackRoot = Join-Path $RepoRoot 'pack'

# The internal OAR folder name is NOT the mod name and must not be renamed to match it. Every
# Thunderchild alias resolves overrideAnimationsFolder relative to this directory, so renaming it
# silently breaks all 29 of them.
$OarRoot  = 'meshes\actors\character\animations\OpenAnimationReplacer\Thuum Reborn'

# Read the version from CMakeLists rather than carrying a second copy of it. Two places to edit is
# how a folder name ends up disagreeing with the DLL's version resource.
$cmake = Get-Content (Join-Path $RepoRoot 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$') {
    throw "Could not read VERSION from CMakeLists.txt -- refusing to guess it."
}
$Version = $Matches[1]

# THE ARTIFACT MUST NAME ITS COMMIT. A rebuild mid-development once overwrote the accepted
# 1.0.0 zip in place, passed every assertion here (the version had not moved), and was then read
# back as the accepted release. The version resource ties the artifact to CMakeLists; nothing tied
# it to a COMMIT, and mtimes are not a record anyone reads under pressure. So: the commit is read
# here, a dirty tree refuses to package (override with -AllowDirty, which stamps "+dirty"), and
# the hash goes into the shipped README and the deployed meta.ini.
$Commit = (& git -C $RepoRoot rev-parse --short=12 HEAD 2>$null)
if (-not $Commit) { throw 'Could not read the git commit -- packaging requires the repository.' }
$DirtyFiles = & git -C $RepoRoot status --porcelain
if ($DirtyFiles) {
    if (-not $AllowDirty) {
        throw "The working tree has uncommitted changes -- an artifact built from it cannot be attributed to a commit. Commit first, or re-run with -AllowDirty to stamp it '+dirty'."
    }
    $Commit = "$Commit+dirty"
}

Write-Host "$ModName $Version (commit $Commit)"

# THE ALLOW-LIST. Source path -> path inside the mod folder. Nothing else ships.
$Payload = @(
    @{ From = 'dist\SKSE\Plugins\ShoutMCO.dll'; To = 'SKSE\Plugins\ShoutMCO.dll'; Required = $true }
    # Shipped from config/, not from dist/. dist/ holds whatever the last live session left behind
    # -- these settings are re-read per shout precisely so they can be edited in place mid-run
    # -- so staging from there would ship somebody's debugging state as the defaults.
    @{ From = 'config\ShoutMCO.ini';            To = 'SKSE\Plugins\ShoutMCO.ini'; Required = $true }
    @{ From = 'docs\release\README.txt';        To = 'README.txt';                Required = $true }
    @{ From = 'LICENSE';                        To = 'LICENSE';                   Required = $true }
)
# ShoutMCO.pdb is deliberately absent: 21MB of symbols against a 616KB DLL. Anyone diagnosing a
# crash log can be handed the matching .pdb from dist/ on request.

# THE C3 PATCH TREE. Source -> path inside the mod folder.
$PatchTree = @{
    From = 'nemesis\Nemesis_Engine'
    To   = 'Nemesis_Engine'
}

# FENCED BY PATH SHAPE, NOT BY FILENAME. A Nemesis mod directory has exactly two file shapes, each
# at a fixed depth below `Nemesis_Engine`:
#
#   mod\<slug>\info.ini                 -- one per mod, naming it in the Nemesis mod list
#   mod\<slug>\<behavior>\#<node>.txt   -- one per patched behavior node
#
# Matching the basename alone was not a fence: `notes\#scratch.txt` or a stray nested `info.ini`
# passed it and reached the archive, because nothing checked where the file actually sat. Cold
# review of b82d547 raised that as a PRODUCT finding and it was right.
function Test-NemesisPatchPath {
    param([string]$Relative)
    $seg = @($Relative -split '\\')
    if ($seg[0] -ne 'mod') { return $false }
    if ($seg.Count -eq 3) { return $seg[2] -eq 'info.ini' }
    if ($seg.Count -eq 4) { return $seg[3] -like '#*.txt' }
    return $false
}

# THE ANIMATION TREE IS FENCED BY SHAPE for the same reason the Nemesis tree is: the pack is 216
# clips and 48 configs across 47 submods, 29 of them added in one pass, so a hand-maintained
# file list is one somebody forgets to update. Below the replacer-mod root an OAR tree has exactly
# these shapes:
#
#   config.json                     -- the replacer mod's own root config
#   <submod>\config.json            -- one per submod
#   <submod>\<clip>.hkx             -- the clips that submod replaces
#
# Checked on the whole relative path, not the basename, so a stray notes\config.json cannot pass.
function Test-OarPackPath {
    param([string]$Relative)
    $seg = @($Relative -split '\\')
    if ($seg.Count -eq 1) { return $seg[0] -eq 'config.json' }
    if ($seg.Count -eq 2) { return ($seg[1] -eq 'config.json') -or ($seg[1] -like '*.hkx') }
    return $false
}

$StageRoot = Join-Path $RepoRoot 'release'
$StageDir  = Join-Path $StageRoot "$ModName $Version"

# EVERYTHING IS VALIDATED BEFORE THE STAGE DIRECTORY IS TOUCHED, and the ordering is the point.
# `$StageDir` is DELETED before it is rebuilt, so a throw partway through staging does not merely
# leave a half-built folder -- it destroys a previously complete one and leaves the same-version
# archive beside it, still describing the old contents. The patch-tree check used to run after the
# allow-list files had already been copied, which is exactly that hazard; the DLL's own
# missing-file throw sat inside the copy loop and had it too, and is moved here with it.
foreach ($item in $Payload) {
    if ($item.Required -and -not (Test-Path (Join-Path $RepoRoot $item.From))) {
        throw "Missing required payload file: $($item.From). Build first (cmake --build --preset ALL-Release)."
    }
}

$patchSrc = Join-Path $RepoRoot $PatchTree.From
if (-not (Test-Path $patchSrc)) { throw "Missing required payload tree: $($PatchTree.From)." }

$patchFiles = @(Get-ChildItem $patchSrc -Recurse -File)
if ($patchFiles.Count -eq 0) { throw "Payload tree $($PatchTree.From) is empty -- refusing to ship a Nemesis mod with no patch files." }

$patchRelative = @($patchFiles | ForEach-Object { $_.FullName.Substring($patchSrc.Length + 1) })

$stray = @($patchRelative | Where-Object { -not (Test-NemesisPatchPath $_) })
if ($stray.Count -gt 0) {
    throw @"
$($stray.Count) file(s) under $($PatchTree.From) are not Nemesis patch files in a Nemesis patch
location, and this script ships that directory whole. Expected mod\<slug>\info.ini or
mod\<slug>\<behavior>\#<node>.txt. Remove them, or widen Test-NemesisPatchPath deliberately:
$($stray | ForEach-Object { "  - $_" } | Out-String)
"@
}

# One info.ini per slug, and no slug that is nothing but an info.ini. Nemesis lists a mod from its
# info.ini and then finds nothing to patch, which fails as a silently absent patch rather than as
# an error -- the failure mode this whole ticket exists to avoid.
$slugs = @($patchRelative | ForEach-Object { ($_ -split '\\')[1] } | Sort-Object -Unique)
foreach ($slug in $slugs) {
    $own = @($patchRelative | Where-Object { ($_ -split '\\')[1] -eq $slug })
    $infos = @($own | Where-Object { ($_ -split '\\').Count -eq 3 })
    $nodes = @($own | Where-Object { ($_ -split '\\').Count -eq 4 })
    if ($infos.Count -ne 1) { throw "Nemesis mod '$slug' has $($infos.Count) info.ini files; it needs exactly one." }
    if ($nodes.Count -eq 0) { throw "Nemesis mod '$slug' has an info.ini but no #<node>.txt patch files -- Nemesis would list it and patch nothing." }
}

# THE PACK VERIFIER IS A GATE, not a nicety. It checks that every family has its twelve clips, that
# every alias borrows a family that exists, that no stray HKX sits inside an alias, that each alias
# carries exactly one IsEquippedShout keyed on the defining plugin, and that no two submods share a
# priority. A pack that fails it ships shouts which silently fall through to the Default animation.
$SourceOar = Join-Path $PackRoot $OarRoot
if (-not (Test-Path $SourceOar)) {
    throw "No OAR tree at $SourceOar -- run node tools/author-thuum-family.mjs first."
}
$PackCredits = Join-Path $PackRoot 'CREDITS.txt'
if (-not (Test-Path $PackCredits)) {
    throw "Missing pack/CREDITS.txt. The animations are BOTuser999's (Nexus 50559) and crediting them is a CONDITION of redistributing them -- refusing to package without it."
}
if (-not $SkipPackVerify) {
    Write-Host '  running tools/verify-thuum-pack.mjs'
    $verify = & node (Join-Path $RepoRoot 'tools\verify-thuum-pack.mjs') 2>&1
    $verify | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) { throw 'Pack verifier failed -- refusing to package.' }
}
$oarFiles = @(Get-ChildItem $SourceOar -Recurse -File)
if ($oarFiles.Count -eq 0) { throw "The OAR tree at $SourceOar is empty." }
$illegalOar = @($oarFiles | Where-Object { -not (Test-OarPackPath $_.FullName.Substring($SourceOar.Length + 1)) })
if ($illegalOar.Count -gt 0) {
    throw "Files at illegal OAR paths (expected <submod>\config.json or <submod>\*.hkx):`n  " +
          (($illegalOar | ForEach-Object { $_.FullName.Substring($SourceOar.Length + 1) }) -join "`n  ")
}

if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
New-Item -ItemType Directory -Path $StageDir -Force | Out-Null

foreach ($item in $Payload) {
    $src = Join-Path $RepoRoot $item.From
    if (-not (Test-Path $src)) { continue }   # optional entries only; required ones threw above
    $dst = Join-Path $StageDir $item.To
    New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
    Copy-Item $src $dst -Force
    Write-Host ("  staged {0,-28} -> {1}" -f $item.From, $item.To)
}

foreach ($rel in $patchRelative) {
    $dst = Join-Path $StageDir (Join-Path $PatchTree.To $rel)
    New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
    Copy-Item (Join-Path $patchSrc $rel) $dst -Force
    Write-Host ("  staged {0,-28} -> {1}" -f (Join-Path $PatchTree.From $rel), (Join-Path $PatchTree.To $rel))
}

$StagedOar = Join-Path $StageDir $OarRoot
New-Item -ItemType Directory -Path (Split-Path -Parent $StagedOar) -Force | Out-Null
Copy-Item $SourceOar $StagedOar -Recurse -Force
Copy-Item $PackCredits (Join-Path $StageDir 'CREDITS.txt') -Force
$submodCount = (Get-ChildItem $SourceOar -Directory).Count
$clipCount   = @($oarFiles | Where-Object { $_.Extension -eq '.hkx' }).Count
Write-Host ("  staged {0,-28} -> {1}" -f 'pack animations', "$OarRoot ($submodCount submods, $clipCount clips)")
Write-Host ("  staged {0,-28} -> {1}" -f 'pack\CREDITS.txt', 'CREDITS.txt')

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

# Stamp the provenance into the shipped README, as the last lines of the file. The staged copy is
# stamped, never the source in docs/release/ -- the stamp describes THIS packaging run.
$stagedReadme = Join-Path $StageDir 'README.txt'
@"

----------------------------------------------------------------------
Build: $Version, commit $Commit, packaged $(Get-Date -Format 'yyyy-MM-dd HH:mm') by tools/package.ps1.
"@ | Add-Content -Path $stagedReadme -Encoding UTF8
Write-Host "  stamped README.txt: $Version, commit $Commit"

# The MOD FOLDER keeps the real name, apostrophe and all -- that is what MO2 and the player see.
# The ARCHIVE does not: a naive "replace spaces with dashes" turned "Thu'um - Fully Animated
# Shouts Reborn" into "Thu'um---Fully-Animated-Shouts-Reborn", and an apostrophe in a download
# filename is a nuisance in browsers, shells and mod managers alike. So drop apostrophes, collapse
# the " - " separator, and squeeze any run of dashes back to one.
$ArchiveStem = ($ModName -replace "'", '' -replace '\s*-\s*', ' ' -replace '\s+', '-' -replace '-+', '-')
$Archive = Join-Path $StageRoot "$ArchiveStem-$Version.zip"
if (Test-Path $Archive) { Remove-Item $Archive -Force }
Compress-Archive -Path (Join-Path $StageDir '*') -DestinationPath $Archive

Write-Host "`nArchive: $Archive"
Write-Host 'Contents:'
$entries = [System.IO.Compression.ZipFile]::OpenRead($Archive)
try {
    $entries.Entries | ForEach-Object { Write-Host ("  {0,-34} {1,9} bytes" -f $_.FullName, $_.Length) }
    # Belt and braces over the allow-list: assert the outcome, not the intent -- checked by
    # listing the archive rather than by trusting the packaging step.
    $forbidden = $entries.Entries | Where-Object { $_.FullName -match '(?i)(^|/)(Thuum|extern|build|\.scratch)/' -or $_.FullName -match '(?i)\.(pdb|esp|esl|esm|psc|pex)$' }
    if ($forbidden) { throw "Archive contains files that must never ship: $($forbidden.FullName -join ', ')" }
} finally {
    $entries.Dispose()
}
Write-Host 'Verified: no Thuum/, no extern/, no build/, no .scratch/, no .pdb, no ESP, no Papyrus.'

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
comments=$ModName $Version, commit $Commit -- packaged by tools/package.ps1
"@ | Set-Content (Join-Path $Target 'meta.ini') -Encoding UTF8

Write-Host "`nDeployed. Enable '$ModName' in MO2 and place it after ADXP - MCO."
Get-ChildItem $Target -Recurse -File | ForEach-Object { Write-Host "  $($_.FullName.Substring($Target.Length + 1))" }
