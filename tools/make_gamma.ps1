# Draws host\assets\gamma.png, the picture behind the Gamma screen: a synthetic test card, so the file is safe to keep in the repository.
# Top: a smooth ramp and 16 steps. Bottom left: near-black squares (the first few should only just show). Bottom middle: lines of black and white beside a solid
# 50% grey, which match when the display is at gamma 2.2. Bottom right: colour patches. The middle is kept dark so the menu text stays readable.
param([string]$Out = (Join-Path $PSScriptRoot '..\host\assets\gamma.png'))
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap 1280, 720, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::Black)
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
$font = New-Object System.Drawing.Font 'Segoe UI', 13
$white = [System.Drawing.Brushes]::Gainsboro
function Grey([int]$v) { [System.Drawing.Color]::FromArgb($v, $v, $v) }
# ramp
for ($x = 0; $x -lt 1100; $x++) { $v = [int][Math]::Round($x * 255 / 1099); $p = New-Object System.Drawing.Pen (Grey $v); $g.DrawLine($p, 90 + $x, 50, 90 + $x, 110); $p.Dispose() }
$g.DrawString('0', $font, $white, 60, 68); $g.DrawString('255', $font, $white, 1195, 68)
# 16 steps
for ($i = 0; $i -lt 16; $i++) { $v = [int][Math]::Round($i * 255 / 15); $b = New-Object System.Drawing.SolidBrush (Grey $v); $g.FillRectangle($b, 90 + $i * 68.75, 130, 68, 90); $b.Dispose() }
# near-black squares: 2 to 16 percent steps
$g.DrawString('Darkest squares: the first few should barely show', $font, $white, 90, 440)
for ($i = 0; $i -lt 8; $i++) { $v = 4 + $i * 4; $b = New-Object System.Drawing.SolidBrush (Grey $v); $g.FillRectangle($b, 90 + $i * 52, 480, 44, 100); $b.Dispose() }
# lines against grey
$g.DrawString('Lines and grey: match at gamma 1.00 on a standard display', $font, $white, 540, 440)
for ($y = 480; $y -lt 580; $y++) { $v = if ((($y - 480) % 2) -eq 0) { 255 } else { 0 }; $p = New-Object System.Drawing.Pen (Grey $v); $g.DrawLine($p, 540, $y, 700, $y); $p.Dispose() }
$b = New-Object System.Drawing.SolidBrush (Grey 186); $g.FillRectangle($b, 700, 480, 160, 100); $b.Dispose()
for ($y = 480; $y -lt 580; $y++) { $v = if ((($y - 480) % 2) -eq 0) { 255 } else { 0 }; $p = New-Object System.Drawing.Pen (Grey $v); $g.DrawLine($p, 860, $y, 1020, $y); $p.Dispose() }
# colours
$cols = @(@(200, 40, 40), @(40, 170, 60), @(50, 80, 210), @(224, 172, 140), @(240, 200, 60))
for ($i = 0; $i -lt $cols.Count; $i++) { $c = $cols[$i]; $b = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($c[0], $c[1], $c[2])); $g.FillRectangle($b, 90 + $i * 215, 600, 200, 44); $b.Dispose() }
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"wrote $Out"

