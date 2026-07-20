$ErrorActionPreference = "Stop"

$Cli = "D:\ESP32Projects\tools\arduino-cli\arduino-cli.exe"
$Config = "D:\ESP32Projects\tools\arduino-cli.yaml"

& $Cli --config-file $Config core update-index
& $Cli --config-file $Config core install esp32:esp32@3.3.10
& $Cli --config-file $Config core list
