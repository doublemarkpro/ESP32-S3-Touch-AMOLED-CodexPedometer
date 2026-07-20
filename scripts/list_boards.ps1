$ErrorActionPreference = "Stop"

$Cli = "D:\ESP32Projects\tools\arduino-cli\arduino-cli.exe"
$Config = "D:\ESP32Projects\tools\arduino-cli.yaml"

& $Cli --config-file $Config board list
