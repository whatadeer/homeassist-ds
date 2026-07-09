#
# Builds the project inside the devkitARM container - no local toolchain needed.
#
# Usage:
#   .\build.ps1          # make cia  (default)
#   .\build.ps1 3dsx      # make build/ha3ds.3dsx only
#   .\build.ps1 clean
#
param(
    [string]$Target = "cia"
)

$ErrorActionPreference = "Stop"

if ($Target -eq "3dsx") {
    # Must be the real target name, not "" - an empty argument makes `make`
    # fall back to its default goal, which is `banner.bnr` (the first rule
    # textually in the Makefile), not the 3dsx/elf build.
    $Target = "all"
}

podman build -t ha3ds-builder -f "$PSScriptRoot\Containerfile" $PSScriptRoot
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

podman run --rm -v "${PSScriptRoot}:/project" ha3ds-builder make $Target
exit $LASTEXITCODE
