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
    $Target = ""
}

podman build -t ha3ds-builder -f "$PSScriptRoot\Containerfile" $PSScriptRoot
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

podman run --rm -v "${PSScriptRoot}:/project" ha3ds-builder make $Target
exit $LASTEXITCODE
