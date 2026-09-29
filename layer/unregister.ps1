# Removes every PimaxGazeLayer manifest registration from the current user's implicit layers.
$key = "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
if (Test-Path $key) {
    $props = Get-ItemProperty $key
    foreach ($p in $props.PSObject.Properties) {
        if ($p.Name -like "*XR_APILAYER_PIMAXGAZE_eye_gaze.json") {
            Remove-ItemProperty -Path $key -Name $p.Name
            "unregistered: $($p.Name)"
        }
    }
}
