param(
    [Parameter(Mandatory = $true)]
    [string]$Port
)

$ErrorActionPreference = "Stop"

$ProjectRoot = "D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75"
$Cli = "D:\ESP32Projects\tools\arduino-cli\arduino-cli.exe"
$Config = "D:\ESP32Projects\tools\arduino-cli.yaml"
$Sketch = Join-Path $ProjectRoot "examples\arduino\11_CodexPedometer"
$Libraries = Join-Path $ProjectRoot "examples\arduino\libraries"
$Fqbn = "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc"

& $Cli --config-file $Config upload -p $Port --fqbn $Fqbn --libraries $Libraries $Sketch
