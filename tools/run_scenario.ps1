<#
.SYNOPSIS
  Runs gr-port test scenarios (tools/scenarios.json) and checks the captured frames.

.DESCRIPTION
  Each scenario starts the game with its own options (for example --skip-opening and --warp to jump
  straight to a mission), plays a scripted pad input, captures the listed guest frames and exits as
  soon as the last one is written. Results go to build\bin\test_out\<scenario>\:
    frame_NNNNN.png   the captured frames
    sheet.png         contact sheet (captured frames, with the reference frames above when present)
    trace.txt         the loader trace of the run
  With -SaveReference the frames become the reference set (build\bin\test_ref\<scenario>\, kept out
  of the repository because they are images of the game). Without it, frames are compared against
  the references (mean absolute difference of 320x180 thumbnails, 0-255) and the run fails above
  -Threshold.

  Input syntax (the loader's --input): "seconds:BUTTON[:duration],..."; this script also expands
  "@from-to/step:BUTTON[:duration]" into a press every <step> seconds.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run_scenario.ps1 ep03
  powershell -ExecutionPolicy Bypass -File tools\run_scenario.ps1 all -SaveReference
#>
param(
    [Parameter(Position = 0)][string[]]$Name = @("all"),
    [switch]$SaveReference,
    [double]$Threshold = 12,
    [string]$Game = $env:GR_UROOT,
    [switch]$Window
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo "build\bin"
$exe = Join-Path $bin "gr_port.exe"
if (-not $Game) { $Game = Join-Path (Split-Path -Parent $repo) "CUSA01113_v2\uroot" }
if (-not (Test-Path $exe)) { throw "gr_port.exe not found in $bin (build first)" }
if (-not (Test-Path $Game)) { throw "game directory not found: $Game (pass -Game or set GR_UROOT)" }
Add-Type -AssemblyName System.Drawing

$all = (Get-Content (Join-Path $PSScriptRoot "scenarios.json") -Raw | ConvertFrom-Json).scenarios
$names = if ($Name -contains "all") { $all.PSObject.Properties.Name } else { $Name }

function Expand-Input([string]$s) {
    $out = @()
    foreach ($tok in ($s -split ",")) {
        if (-not $tok) { continue }
        if ($tok -match '^@([\d.]+)-([\d.]+)/([\d.]+):(.+)$') {
            for ($t = [double]$Matches[1]; $t -le [double]$Matches[2]; $t += [double]$Matches[3]) {
                $out += ("{0}:{1}" -f $t.ToString([Globalization.CultureInfo]::InvariantCulture), $Matches[4])
            }
        } else { $out += $tok }
    }
    return ($out -join ",")
}

function Get-Thumb([string]$path) {
    $img = [System.Drawing.Image]::FromFile($path)
    $t = New-Object System.Drawing.Bitmap 320, 180
    $g = [System.Drawing.Graphics]::FromImage($t)
    $g.InterpolationMode = "HighQualityBilinear"
    $g.DrawImage($img, 0, 0, 320, 180)
    $g.Dispose(); $img.Dispose()
    return $t
}

function Get-Diff([string]$a, [string]$b) {
    $ta = Get-Thumb $a; $tb = Get-Thumb $b
    $rect = New-Object System.Drawing.Rectangle 0, 0, 320, 180
    $da = $ta.LockBits($rect, "ReadOnly", "Format32bppArgb"); $db = $tb.LockBits($rect, "ReadOnly", "Format32bppArgb")
    $n = $da.Stride * 180
    $ba = New-Object byte[] $n; $bb = New-Object byte[] $n
    [Runtime.InteropServices.Marshal]::Copy($da.Scan0, $ba, 0, $n)
    [Runtime.InteropServices.Marshal]::Copy($db.Scan0, $bb, 0, $n)
    $ta.UnlockBits($da); $tb.UnlockBits($db); $ta.Dispose(); $tb.Dispose()
    $sum = 0L
    for ($i = 0; $i -lt $n; $i += 4) {
        $sum += [Math]::Abs($ba[$i] - $bb[$i]) + [Math]::Abs($ba[$i + 1] - $bb[$i + 1]) + [Math]::Abs($ba[$i + 2] - $bb[$i + 2])
    }
    return [Math]::Round($sum / ($n / 4 * 3), 2)
}

function Save-Sheet([string]$dir, [string[]]$frames, [string]$refDir) {
    $cols = [Math]::Max(1, $frames.Count); $rows = if ($refDir) { 2 } else { 1 }
    $sheet = New-Object System.Drawing.Bitmap ($cols * 480), ($rows * 290)
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.Clear([System.Drawing.Color]::Black)
    $font = New-Object System.Drawing.Font "Arial", 11
    for ($c = 0; $c -lt $frames.Count; $c++) {
        $f = $frames[$c]
        $sets = @(@{ dir = $dir; label = "now" })
        if ($refDir) { $sets = @(@{ dir = $refDir; label = "ref" }) + $sets }
        for ($r = 0; $r -lt $sets.Count; $r++) {
            $p = Join-Path $sets[$r].dir $f
            if (Test-Path $p) {
                $img = [System.Drawing.Image]::FromFile($p)
                $g.DrawImage($img, $c * 480, $r * 290 + 20, 480, 270)
                $img.Dispose()
            }
            $g.DrawString("$($sets[$r].label) $f", $font, [System.Drawing.Brushes]::Yellow, $c * 480 + 4, $r * 290 + 2)
        }
    }
    $sheet.Save((Join-Path $dir "sheet.png"))
    $g.Dispose(); $sheet.Dispose()
}

$failed = 0
foreach ($n in $names) {
    $sc = $all.$n
    if (-not $sc) { Write-Host "unknown scenario '$n'"; $failed++; continue }
    $out = Join-Path $bin "test_out\$n"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory -Force $out | Out-Null
    $frames = @($sc.frames | ForEach-Object { "{0}" -f $_ })
    $argv = @("`"$Game`"", "--run", "--mute", "--dump-at", ($frames -join ","), "--dump-dir", "`"$out`"", "--exit-after-dumps",
              "--report", "`"$(Join-Path $out 'report.txt')`"")
    if (-not $Window) { $argv += @("--no-window", "--timeout", "$([int]$sc.seconds + 10)") }
    # every run starts from a copy of the saves (build\bin\savedata), so tests never touch the real ones
    $saves = Join-Path $out "savedata"
    if (Test-Path (Join-Path $bin "savedata")) { Copy-Item -Recurse (Join-Path $bin "savedata") $saves }
    else { New-Item -ItemType Directory -Force $saves | Out-Null }
    $argv += @("--savedata", "`"$saves`"")
    $argv += @($sc.args)
    $inp = Expand-Input $sc.input
    if ($inp) { $argv += @("--input", "`"$inp`"") }
    Write-Host "== $n : $($sc.description)"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -ArgumentList $argv -WorkingDirectory $out -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(([int]$sc.seconds + 20) * 1000)) { Stop-Process -Id $p.Id -Force; Write-Host "   timeout" }
    $sw.Stop()
    if (Test-Path (Join-Path $out "gr_port_trace.txt")) { Move-Item (Join-Path $out "gr_port_trace.txt") (Join-Path $out "trace.txt") -Force }
    $names_png = $frames | ForEach-Object { "frame_{0:D5}.png" -f [int]$_ }
    $ref = Join-Path $bin "test_ref\$n"
    $missing = @($names_png | Where-Object { -not (Test-Path (Join-Path $out $_)) })
    Write-Host ("   {0:N0} s, {1}/{2} frames" -f $sw.Elapsed.TotalSeconds, ($names_png.Count - $missing.Count), $names_png.Count)
    if ($missing.Count) { Write-Host "   FAIL: missing $($missing -join ', ')"; $failed++ }
    if ($SaveReference) {
        New-Item -ItemType Directory -Force $ref | Out-Null
        foreach ($f in $names_png) { if (Test-Path (Join-Path $out $f)) { Copy-Item (Join-Path $out $f) $ref -Force } }
        Write-Host "   reference saved to $ref"
        Save-Sheet $out $names_png $null
    } elseif (Test-Path $ref) {
        foreach ($f in $names_png) {
            if (-not (Test-Path (Join-Path $out $f)) -or -not (Test-Path (Join-Path $ref $f))) { continue }
            $d = Get-Diff (Join-Path $ref $f) (Join-Path $out $f)
            $ok = $d -le $Threshold
            if (-not $ok) { $failed++ }
            Write-Host ("   {0}  diff {1,6}  {2}" -f $f, $d, $(if ($ok) { "ok" } else { "FAIL" }))
        }
        Save-Sheet $out $names_png $ref
    } else {
        Write-Host "   no reference (run with -SaveReference to create one)"
        Save-Sheet $out $names_png $null
    }
    Write-Host "   sheet: $(Join-Path $out 'sheet.png')"
}
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host "all checks passed"
