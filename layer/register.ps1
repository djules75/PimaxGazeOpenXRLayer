# Registers the layer as an OpenXR implicit layer for the current user.
# Run from anywhere; it registers the manifest that sits next to this script's
# build output (pass -ManifestPath to point elsewhere).
param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot "build\Release\XR_APILAYER_PIMAXGAZE_eye_gaze.json")
)
$ManifestPath = (Resolve-Path $ManifestPath).Path
$key = "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
New-ItemProperty -Path $key -Name $ManifestPath -Value 0 -PropertyType DWord -Force | Out-Null
"registered: $ManifestPath"
Get-ItemProperty $key | Select-Object -Property * -ExcludeProperty PS* | Format-List
