# Bakes the button prompt atlases from the Kenney Input Prompts pack (CC0, https://kenney.nl/assets/input-prompts), extracted under assets_src.
# Each output is a replacement for the game's 256x256 button atlas at 3x: every glyph is fitted to the bounds the original art has in that atlas, so the
# game's own glyph rectangles still land on it. Output: host\assets\buttons\<set>.png
param([string]$Src = (Join-Path $PSScriptRoot '..\assets_src'), [string]$Out = (Join-Path $PSScriptRoot '..\host\assets\buttons'), [int]$Scale = 3)
Add-Type -AssemblyName System.Drawing

# Bounds of each glyph in the original atlas (x0, y0, x1 exclusive, y1 exclusive), measured from a dump of the game's texture.
$bounds = [ordered]@{
    lt = 11, 11, 59, 55;  rt = 70, 11, 118, 55;  lb = 131, 11, 190, 52;  rb = 196, 11, 255, 52
    back = 9, 70, 78, 122;  rstick = 139, 77, 183, 121;  lstick = 202, 77, 246, 121
    a = 10, 141, 54, 185;  b = 74, 141, 118, 185;  x = 138, 141, 182, 185;  y = 202, 141, 246, 185
    start = 136, 198, 205, 250
}
# Which Kenney file shows each of the game's buttons. The game's A is its bottom face button, so the Switch set follows position (game A = Switch B).
$sets = [ordered]@{
    xbox = @{ dir = 'Xbox Series'; files = @{
        a = 'xbox_button_color_a'; b = 'xbox_button_color_b'; x = 'xbox_button_color_x'; y = 'xbox_button_color_y'
        lb = 'xbox_lb'; rb = 'xbox_rb'; lt = 'xbox_lt'; rt = 'xbox_rt'; back = 'xbox_button_back_icon'; start = 'xbox_button_start_icon'
        lstick = 'xbox_stick_l'; rstick = 'xbox_stick_r' } }
    playstation = @{ dir = 'PlayStation Series'; files = @{
        a = 'playstation_button_color_cross'; b = 'playstation_button_color_circle'; x = 'playstation_button_color_square'; y = 'playstation_button_color_triangle'
        lb = 'playstation_trigger_l1'; rb = 'playstation_trigger_r1'; lt = 'playstation_trigger_l2'; rt = 'playstation_trigger_r2'
        back = 'playstation5_button_create'; start = 'playstation5_button_options'; lstick = 'playstation_stick_l'; rstick = 'playstation_stick_r' } }
    switch = @{ dir = 'Nintendo Switch'; files = @{
        a = 'switch_button_b'; b = 'switch_button_a'; x = 'switch_button_y'; y = 'switch_button_x'
        lb = 'switch_button_l'; rb = 'switch_button_r'; lt = 'switch_button_zl'; rt = 'switch_button_zr'
        back = 'switch_button_minus'; start = 'switch_button_plus'; lstick = 'switch_stick_l'; rstick = 'switch_stick_r' } }
}
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($name in $sets.Keys) {
    $set = $sets[$name]
    $atlas = New-Object Drawing.Bitmap (256 * $Scale), (256 * $Scale), ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($atlas)
    $g.Clear([Drawing.Color]::FromArgb(0, 0, 0, 0))
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    foreach ($glyph in $bounds.Keys) {
        $path = Join-Path $Src "$($set.dir)\Double\$($set.files[$glyph]).png"
        if (-not (Test-Path $path)) { throw "missing $path" }
        $img = New-Object Drawing.Bitmap $path
        # The glyph's own opaque area, so margins in the pack's art don't change its size.
        $minx = $img.Width; $miny = $img.Height; $maxx = -1; $maxy = -1
        for ($yy = 0; $yy -lt $img.Height; $yy++) { for ($xx = 0; $xx -lt $img.Width; $xx++) { if ($img.GetPixel($xx, $yy).A -gt 20) { if ($xx -lt $minx) { $minx = $xx }; if ($xx -gt $maxx) { $maxx = $xx }; if ($yy -lt $miny) { $miny = $yy }; if ($yy -gt $maxy) { $maxy = $yy } } } }
        $sw = $maxx - $minx + 1; $sh = $maxy - $miny + 1
        $b = $bounds[$glyph]
        $bx = $b[0] * $Scale; $by = $b[1] * $Scale; $bw = ($b[2] - $b[0]) * $Scale; $bh = ($b[3] - $b[1]) * $Scale
        $f = [Math]::Min($bw / $sw, $bh / $sh)
        $dw = [int][Math]::Round($sw * $f); $dh = [int][Math]::Round($sh * $f)
        $dx = $bx + [int][Math]::Floor(($bw - $dw) / 2); $dy = $by + [int][Math]::Floor(($bh - $dh) / 2)
        $g.DrawImage($img, (New-Object Drawing.Rectangle $dx, $dy, $dw, $dh), (New-Object Drawing.Rectangle $minx, $miny, $sw, $sh), [Drawing.GraphicsUnit]::Pixel)
        $img.Dispose()
    }
    $g.Dispose()
    $file = Join-Path $Out "$name.png"
    $atlas.Save($file, [Drawing.Imaging.ImageFormat]::Png)
    $atlas.Dispose()
    "{0}: {1}" -f $name, $file
}
Set-Content -Encoding UTF8 (Join-Path $Out 'CREDITS.txt') @'
Button prompt art: Kenney Input Prompts, https://kenney.nl/assets/input-prompts (c) Kenney, licensed CC0 1.0 Universal (public domain).
The sets here (xbox.png, playstation.png, switch.png) are rearranged copies of its Xbox Series, PlayStation Series and Nintendo Switch glyphs, built by tools/bake_buttons.ps1.
'@
