@echo off
setlocal

set SCRIPT_DIR=%~dp0

set DDKROOT=C:\98DDK
if not "%1"=="" set DDKROOT=%1
set DDKROOT=%DDKROOT:"=%

set BUILD_KIND=free
if not "%2"=="" set BUILD_KIND=%2

if not exist "%DDKROOT%\bin\SETENV.BAT" (
    echo error: could not find %DDKROOT%\bin\SETENV.BAT
    exit /b 1
)

call "%DDKROOT%\bin\SETENV.BAT" %DDKROOT% %BUILD_KIND%
if errorlevel 1 exit /b %errorlevel%

cd /d "%SCRIPT_DIR%"
if errorlevel 1 (
    echo error: could not change to %SCRIPT_DIR%
    exit /b 1
)

where nmake >nul 2>nul
if errorlevel 1 (
    echo error: nmake was not found after SETENV.BAT.
    echo hint: the Windows 98 DDK expects an MSVC 4.2/5.0/6.0 style toolchain via MSDEVDIR.
    echo hint: VC6 should be installed at C:\VC6 and MSDEVDIR should point to it.
    exit /b 1
)

echo building VMODEM.vxd from %SCRIPT_DIR%
nmake /nologo /a
if errorlevel 1 exit /b %errorlevel%

if not exist "obj\i386\VMODEM.vxd" (
    echo error: build completed without producing obj\i386\VMODEM.vxd
    exit /b 1
)

echo success: built %SCRIPT_DIR%obj\i386\VMODEM.vxd
exit /b 0
