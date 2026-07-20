$ErrorActionPreference = "Stop"

$Python = "C:\Users\mark\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
$Server = "D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75\tools\codex_usage_server.py"

& $Python $Server
