# Makes the key icons that Kenney's set does not have: left and right Shift, Ctrl and Alt (the plain icon with a small L or R tag) and the numpad digits (the digit key with a NUM tag).
# Reads assets_src\Keyboard & Mouse\Default and writes host\assets\keys.
param([string]$Src = (Join-Path $PSScriptRoot '..\assets_src\Keyboard & Mouse\Default'), [string]$Dst = (Join-Path $PSScriptRoot '..\host\assets\keys'))
Add-Type -AssemblyName System.Drawing
function Tag($srcName, $dstName, $text) {
    $im = [System.Drawing.Image]::FromFile((Join-Path $Src "$srcName.png"))
    $bmp = New-Object System.Drawing.Bitmap $im.Width, $im.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.DrawImage($im, 0, 0, $im.Width, $im.Height)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $font = New-Object System.Drawing.Font 'Segoe UI', 17, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $size = $g.MeasureString($text, $font)
    $w = [int]($size.Width + 4); $h = 22
    $x = $im.Width - $w - 2; $y = 2
    $g.FillRectangle([System.Drawing.Brushes]::Black, $x, $y, $w, $h)
    $g.DrawString($text, $font, [System.Drawing.Brushes]::White, $x + 1, $y - 2)
    $g.Dispose()
    $bmp.Save((Join-Path $Dst "$dstName.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose(); $im.Dispose()
}
foreach ($m in 'shift', 'ctrl', 'alt') { Tag "keyboard_$m" "keyboard_${m}_left" 'L'; Tag "keyboard_$m" "keyboard_${m}_right" 'R' }
foreach ($d in 0..9) { Tag "keyboard_$d" "keyboard_numpad_$d" 'NUM' }
foreach ($n in 'keyboard_delete', 'keyboard_end', 'keyboard_home', 'keyboard_page_up', 'keyboard_page_down', 'keyboard_insert', 'keyboard_numpad_plus') { Copy-Item (Join-Path $Src "$n.png") (Join-Path $Dst "$n.png") -Force }
"done"

