@echo off
rem Flash the KX3 / DigiRig Mobile firmware (branch kx3) onto an M5Stack Tab5.
rem
rem Usage:   flash_kx3.bat [COMPORT]        e.g.  flash_kx3.bat COM7
rem          With no argument every serial port is tried, lowest number first.
rem
rem Where it finds the firmware (first match wins):
rem   1. beside this script   - bootloader.bin, partition-table.bin,
rem                             qmx_panadapter.bin, ota_data_initial.bin
rem                             (a copied-out flash_kx3 folder)
rem   2. ..\build\            - the idf.py output of THIS checkout, i.e.
rem                             build\bootloader\bootloader.bin and friends.
rem
rem Needs only Python + esptool (pip install esptool). No ESP-IDF required.
rem
rem *** POWER-CYCLE THE RADIO AFTER THIS. Every flash wedges it (#74). ***
setlocal enabledelayedexpansion
title QMX Panadapter - Tab5 KX3 Flasher

echo(
echo ============================================================
echo    QMX Panadapter KX3 / DigiRig firmware flasher  ^(Tab5^)
echo ============================================================
echo(

rem --- 1. locate the firmware -------------------------------------------------
rem Search order (first hit wins):
rem   a. beside this script
rem   b. %KX3_FW_DIR%               (set it to point anywhere)
rem   c. ..\build\                  (idf.py build on Windows)
rem   d. the WSL build tree         (~/build/qmx-panadapter/build, found via wslpath)
set "SELF=%~dp0"
set "SRC="
set "BL="
set "PT="
set "APP="
set "OTAD="

call :find "!SELF!"
if not defined SRC if defined KX3_FW_DIR call :find "!KX3_FW_DIR!"
if not defined SRC call :find "!SELF!..\build"
if not defined SRC (
    for /f "delims=" %%W in ('wsl.exe -e sh -c "wslpath -w $HOME/build/qmx-panadapter/build" 2^>nul') do (
        if not defined SRC call :find "%%W"
    )
)

if not defined SRC (
    echo ERROR: no firmware found.
    echo   Looked for the four .bin files in:
    echo     - beside this script
    echo     - %%KX3_FW_DIR%% ^(if set^)
    echo     - !SELF!..\build
    echo     - the WSL tree ~/build/qmx-panadapter/build
    echo   Build first, or copy bootloader.bin, partition-table.bin,
    echo   qmx_panadapter.bin and ota_data_initial.bin next to this script.
    goto :end
)
set "MISSING="
if not exist "!BL!"   ( echo ERROR: missing !BL!   & set "MISSING=1" )
if not exist "!PT!"   ( echo ERROR: missing !PT!   & set "MISSING=1" )
if not exist "!APP!"  ( echo ERROR: missing !APP!  & set "MISSING=1" )
if not exist "!OTAD!" (
    echo ERROR: missing !OTAD!
    echo   Without it the Tab5 can restart on the PREVIOUS firmware even
    echo   though this flash succeeded.
    set "MISSING=1"
)
if defined MISSING goto :end

echo Firmware from: !SRC!
echo   0x2000   bootloader.bin
echo   0x8000   partition-table.bin
echo   0x10000  qmx_panadapter.bin
echo   0x920000 ota_data_initial.bin
echo(

rem --- 2. get esptool ---------------------------------------------------------
set "ESPTOOL="
python -m esptool version >nul 2>nul
if not errorlevel 1 set "ESPTOOL=python -m esptool"
if not defined ESPTOOL (
    where esptool.exe >nul 2>nul && set "ESPTOOL=esptool.exe"
)
if not defined ESPTOOL (
    where esptool >nul 2>nul && set "ESPTOOL=esptool"
)
if not defined ESPTOOL (
    echo ERROR: esptool not found.
    echo   Install it with:   python -m pip install esptool
    goto :end
)

rem esptool v5+ spells it write-flash / default-reset; v4 uses underscores.
set "WRITE_FLASH=write_flash"
set "DEFAULT_RESET=default_reset"
set "HARD_RESET=hard_reset"
set "FLASH_OPTS=--flash_mode dio --flash_freq 40m --flash_size 16MB"
%ESPTOOL% version 2>&1 | findstr /r "v[5-9]\." >nul 2>nul
if not errorlevel 1 (
    set "WRITE_FLASH=write-flash"
    set "DEFAULT_RESET=default-reset"
    set "HARD_RESET=hard-reset"
    set "FLASH_OPTS=--flash-mode dio --flash-freq 40m --flash-size 16MB"
)

echo Before you continue:
echo   1. Plug the Tab5 into this PC with a USB-C DATA cable.
echo   2. Close any serial monitor or other app using the port.
echo   3. A full flash resets saved settings - including the KX3 I/Q dongle
echo      VID:PID and your callsign/grid. Have them ready to re-enter.
echo(
pause

rem --- 3. choose the port(s) --------------------------------------------------
set "PORTS="
if not "%~1"=="" (
    set "PORTS=%~1"
) else (
    set "PLIST=%TEMP%\qmx_kx3_ports_%RANDOM%.txt"
    powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object Length,{ $_ } | Set-Content -Encoding ascii -LiteralPath '!PLIST!'"
    if exist "!PLIST!" (
        for /f "usebackq delims=" %%P in ("!PLIST!") do set "PORTS=!PORTS! %%P"
        del "!PLIST!" >nul 2>nul
    )
)

rem --- 4. flash ---------------------------------------------------------------
echo(
echo Flashing - do NOT unplug the Tab5...
echo(

set "RC=1"
if defined PORTS (
    for %%P in (!PORTS!) do (
        if not "!RC!"=="0" (
            echo   trying %%P ...
            %ESPTOOL% --chip esp32p4 -p %%P -b 460800 --before %DEFAULT_RESET% --after %HARD_RESET% --connect-attempts 1 !WRITE_FLASH! %FLASH_OPTS% 0x2000 "!BL!" 0x8000 "!PT!" 0x10000 "!APP!" 0x920000 "!OTAD!"
            if not errorlevel 1 set "RC=0"
        )
    )
) else (
    %ESPTOOL% --chip esp32p4 -b 460800 --before %DEFAULT_RESET% --after %HARD_RESET% --connect-attempts 1 !WRITE_FLASH! %FLASH_OPTS% 0x2000 "!BL!" 0x8000 "!PT!" 0x10000 "!APP!" 0x920000 "!OTAD!"
    if not errorlevel 1 set "RC=0"
)

echo(
if "!RC!"=="0" (
    echo ============================================================
    echo    SUCCESS - the Tab5 is restarting with the KX3 firmware.
    echo(
    echo    NOW POWER-CYCLE THE RADIO: unplug its USB AND DC power,
    echo    wait a few seconds, reconnect. Every flash wedges it.
    echo ============================================================
) else (
    echo ============================================================
    echo    FLASH FAILED - could not flash the Tab5.
    echo    - Use a USB-C cable that carries DATA, not just power.
    echo    - Close any program using the serial port and retry.
    echo    - Name the port:  flash_kx3.bat COM7
    echo    - Unplug, replug, then re-run this script.
    echo ============================================================
)

:end
echo(
pause
endlocal

exit /b 0

rem :find <dir>  - sets SRC/BL/PT/APP/OTAD if <dir> holds the firmware, either
rem as an idf.py build tree or as a flat folder (or its flash_kx3 subfolder).
:find
set "FD=%~1"
if not defined FD exit /b 1
if "!FD:~-1!"=="\" set "FD=!FD:~0,-1!"
if exist "!FD!\bootloader\bootloader.bin" if exist "!FD!\qmx_panadapter.bin" (
    set "SRC=!FD!\"
    set "BL=!FD!\bootloader\bootloader.bin"
    set "PT=!FD!\partition_table\partition-table.bin"
    set "APP=!FD!\qmx_panadapter.bin"
    set "OTAD=!FD!\ota_data_initial.bin"
    exit /b 0
)
if exist "!FD!\flash_kx3\bootloader.bin" call :find_flat "!FD!\flash_kx3" & exit /b 0
if exist "!FD!\bootloader.bin" call :find_flat "!FD!" & exit /b 0
exit /b 1

:find_flat
set "SRC=%~1\"
set "BL=%~1\bootloader.bin"
set "PT=%~1\partition-table.bin"
set "APP=%~1\qmx_panadapter.bin"
set "OTAD=%~1\ota_data_initial.bin"
exit /b 0