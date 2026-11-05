@echo off
rem Double-click to run the game with a controller. The log goes to pad_play.log beside this file.
cd /d "%~dp0"
set "GAME=E:\games\Bionic Commando - Rearmed 2 (World) (XBLA)"
set BCR2_LUATRACE=1
set BCR2_AUTOINPUT=
echo Starting Bionic Commando Rearmed 2. Close this window or press Ctrl+C to stop.
echo Log: %CD%\pad_play.log
build\bcr2.exe "%GAME%\basefile.bin" "%GAME%\extracted_out" 2> pad_play.log
echo.
echo The game exited with code %ERRORLEVEL%. Last log lines:
powershell -NoProfile -Command "Get-Content 'pad_play.log' -Tail 6"
pause
