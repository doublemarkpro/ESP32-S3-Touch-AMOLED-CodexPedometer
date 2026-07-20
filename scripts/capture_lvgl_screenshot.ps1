param(
    [Parameter(Mandatory = $true)]
    [string]$Port,

    [int]$BaudRate = 2000000,

    [string]$OutputPath,

    [int]$StartupDelayMs = 4000,

    [int]$BeginTimeoutMs = 60000,

    [switch]$SwapBytes
)

$ErrorActionPreference = "Stop"

function Convert-HexLineToBytes {
    param([string]$Hex)

    if (($Hex.Length % 2) -ne 0) {
        throw "Odd-length hex line received."
    }

    $bytes = New-Object byte[] ($Hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16)
    }
    return $bytes
}

function Write-BmpFromRgb565Le {
    param(
        [byte[]]$Rgb565,
        [int]$Width,
        [int]$Height,
        [string]$Path,
        [bool]$Swap
    )

    $rowStride = [int]([Math]::Floor(($Width * 3 + 3) / 4) * 4)
    $imageSize = $rowStride * $Height
    $fileSize = 54 + $imageSize

    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    $bw = New-Object System.IO.BinaryWriter($fs)
    try {
        $bw.Write([UInt16]0x4D42)
        $bw.Write([UInt32]$fileSize)
        $bw.Write([UInt16]0)
        $bw.Write([UInt16]0)
        $bw.Write([UInt32]54)
        $bw.Write([UInt32]40)
        $bw.Write([Int32]$Width)
        $bw.Write([Int32]$Height)
        $bw.Write([UInt16]1)
        $bw.Write([UInt16]24)
        $bw.Write([UInt32]0)
        $bw.Write([UInt32]$imageSize)
        $bw.Write([Int32]2835)
        $bw.Write([Int32]2835)
        $bw.Write([UInt32]0)
        $bw.Write([UInt32]0)

        $row = New-Object byte[] $rowStride
        for ($y = $Height - 1; $y -ge 0; $y--) {
            [Array]::Clear($row, 0, $row.Length)
            for ($x = 0; $x -lt $Width; $x++) {
                $src = ($y * $Width + $x) * 2
                if ($Swap) {
                    $v = ([int]$Rgb565[$src] -shl 8) -bor [int]$Rgb565[$src + 1]
                } else {
                    $v = [int]$Rgb565[$src] -bor ([int]$Rgb565[$src + 1] -shl 8)
                }

                $r5 = ($v -shr 11) -band 0x1F
                $g6 = ($v -shr 5) -band 0x3F
                $b5 = $v -band 0x1F
                $r = [byte][Math]::Floor(($r5 * 255 + 15) / 31)
                $g = [byte][Math]::Floor(($g6 * 255 + 31) / 63)
                $b = [byte][Math]::Floor(($b5 * 255 + 15) / 31)

                $dst = $x * 3
                $row[$dst] = $b
                $row[$dst + 1] = $g
                $row[$dst + 2] = $r
            }
            $bw.Write($row)
        }
    } finally {
        $bw.Close()
        $fs.Close()
    }
}

function Save-LayoutReport {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$RequestedPath
    )

    if (-not $RequestedPath -or [System.IO.Path]::GetExtension($RequestedPath).ToLowerInvariant() -eq ".bmp") {
        $projectRoot = Split-Path -Parent $PSScriptRoot
        $captureDir = Join-Path $projectRoot "captures"
        New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
        $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
        $RequestedPath = Join-Path $captureDir "lvgl_layout_$stamp.txt"
    }

    $Lines | Set-Content -Path $RequestedPath -Encoding UTF8

    $svgPath = [System.IO.Path]::ChangeExtension($RequestedPath, ".svg")
    $begin = $Lines | Where-Object { $_ -match "^__LVGL_LAYOUT_BEGIN__" } | Select-Object -First 1
    $w = 466
    $h = 466
    $cx = 233
    $cy = 233
    $r = 233
    if ($begin -match "screen=(\d+)x(\d+).*round_cx=(\d+) round_cy=(\d+) round_r=(\d+)") {
        $w = [int]$Matches[1]
        $h = [int]$Matches[2]
        $cx = [int]$Matches[3]
        $cy = [int]$Matches[4]
        $r = [int]$Matches[5]
    }

    $svg = New-Object System.Collections.Generic.List[string]
    $svg.Add("<svg xmlns='http://www.w3.org/2000/svg' width='$w' height='$h' viewBox='0 0 $w $h'>")
    $svg.Add("<rect x='0' y='0' width='$w' height='$h' fill='black'/>")
    $svg.Add("<circle cx='$cx' cy='$cy' r='$r' fill='none' stroke='#2dd4bf' stroke-width='2' opacity='0.7'/>")
    $svg.Add("<line x1='$cx' y1='0' x2='$cx' y2='$h' stroke='#334155' stroke-width='1'/>")
    $svg.Add("<line x1='0' y1='$cy' x2='$w' y2='$cy' stroke='#334155' stroke-width='1'/>")

    foreach ($line in $Lines) {
        if ($line -match "^(ARC|OBJ) name=([^ ]+) x1=(-?\d+) y1=(-?\d+) x2=(-?\d+) y2=(-?\d+) w=(\d+) h=(\d+) square=([^ ]+) round=([^ ]+)") {
            $kind = $Matches[1]
            $name = $Matches[2]
            $x1 = [int]$Matches[3]
            $y1 = [int]$Matches[4]
            $ww = [int]$Matches[7]
            $hh = [int]$Matches[8]
            $round = $Matches[10]
            $color = if ($round -eq "OK") { "#38bdf8" } else { "#fb7185" }
            $dash = if ($kind -eq "ARC") { " stroke-dasharray='6 4'" } else { "" }
            $svg.Add("<rect x='$x1' y='$y1' width='$ww' height='$hh' fill='none' stroke='$color' stroke-width='2'$dash/>")
            $tx = $x1 + 3
            $ty = $y1 + 12
            $svg.Add("<text x='$tx' y='$ty' fill='$color' font-size='10' font-family='Consolas,monospace'>$name</text>")
        }
    }
    $svg.Add("</svg>")
    $svg | Set-Content -Path $svgPath -Encoding UTF8

    Write-Host "Saved layout report: $RequestedPath"
    Write-Host "Saved layout preview: $svgPath"
}

function Read-LayoutReport {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [string]$FirstLine,
        [string]$RequestedPath
    )

    $layout = New-Object "System.Collections.Generic.List[string]"
    $layout.Add($FirstLine)
    while ($true) {
        try {
            $line = $Serial.ReadLine().Trim()
        } catch [System.TimeoutException] {
            continue
        }
        $layout.Add($line)
        if ($line -eq "__LVGL_LAYOUT_END__") {
            break
        }
    }
    Save-LayoutReport -Lines $layout -RequestedPath $RequestedPath
}

if (-not $OutputPath) {
    $projectRoot = Split-Path -Parent $PSScriptRoot
    $captureDir = Join-Path $projectRoot "captures"
    New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputPath = Join-Path $captureDir "lvgl_screenshot_$stamp.bmp"
}

$serial = New-Object System.IO.Ports.SerialPort($Port, $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.NewLine = "`n"
$serial.ReadTimeout = 500
$serial.WriteTimeout = 5000
$serial.DtrEnable = $false
$serial.RtsEnable = $false

$width = 0
$height = 0
$stream = New-Object System.IO.MemoryStream

try {
    $serial.Open()
    Write-Host "Opened $Port. Waiting $StartupDelayMs ms for the board/console to settle..."
    $settleDeadline = [DateTime]::UtcNow.AddMilliseconds($StartupDelayMs)
    while ([DateTime]::UtcNow -lt $settleDeadline) {
        try {
            $preLine = $serial.ReadLine().Trim()
            if ($preLine -match "^__LVGL_SCREENSHOT_BEGIN__ width=(\d+) height=(\d+) stride=(\d+) format=RGB565LE$") {
                $width = [int]$Matches[1]
                $height = [int]$Matches[2]
                Write-Host "Receiving $width x $height RGB565 frame..."
                break
            }
            if ($preLine -match "^__LVGL_LAYOUT_BEGIN__") {
                Read-LayoutReport -Serial $serial -FirstLine $preLine -RequestedPath $OutputPath
                return
            }
            if ($preLine -match "^__LVGL_SCREENSHOT_ERROR__") {
                Write-Host "[serial] $preLine"
            }
            if ($preLine.Length -gt 0) {
                Write-Host "[serial] $preLine"
            }
        } catch [System.TimeoutException] {
        }
    }

    if ($width -eq 0 -or $height -eq 0) {
        $serial.WriteLine("shot")

        Write-Host "Waiting for screenshot from $Port at $BaudRate baud..."
        $beginDeadline = [DateTime]::UtcNow.AddMilliseconds($BeginTimeoutMs)

        while ($true) {
            if ([DateTime]::UtcNow -gt $beginDeadline) {
                throw "Timed out waiting for screenshot header. Check that the latest firmware is flashed, the monitor is closed, and $Port is the ESP32 console port."
            }

            try {
                $line = $serial.ReadLine().Trim()
            } catch [System.TimeoutException] {
                continue
            }

            if ($line -match "^__LVGL_SCREENSHOT_BEGIN__ width=(\d+) height=(\d+) stride=(\d+) format=RGB565LE$") {
                $width = [int]$Matches[1]
                $height = [int]$Matches[2]
                Write-Host "Receiving $width x $height RGB565 frame..."
                break
            }
            if ($line -match "^__LVGL_LAYOUT_BEGIN__") {
                Read-LayoutReport -Serial $serial -FirstLine $line -RequestedPath $OutputPath
                return
            }
            if ($line -match "^__LVGL_SCREENSHOT_ERROR__") {
                Write-Host "[serial] $line"
            }
            if ($line.Length -gt 0) {
                Write-Host "[serial] $line"
            }
        }
    }

    while ($true) {
        try {
            $line = $serial.ReadLine().Trim()
        } catch [System.TimeoutException] {
            continue
        }
        if ($line -eq "__LVGL_SCREENSHOT_END__") {
            break
        }
        if ($line -match "^[0-9A-Fa-f]+$") {
            $bytes = Convert-HexLineToBytes $line
            $stream.Write($bytes, 0, $bytes.Length)
        }
    }
} finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
}

$raw = $stream.ToArray()
$expected = $width * $height * 2
if ($raw.Length -ne $expected) {
    throw "Screenshot length mismatch. Expected $expected bytes, got $($raw.Length) bytes."
}

Write-BmpFromRgb565Le -Rgb565 $raw -Width $width -Height $height -Path $OutputPath -Swap ([bool]$SwapBytes)
Write-Host "Saved screenshot: $OutputPath"
