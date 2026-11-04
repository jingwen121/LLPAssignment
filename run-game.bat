@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "WSL_DISTRO=%~1"
set "GAME_ROLE=%~2"
set "DISTRO_CONFIG=%LOCALAPPDATA%\CoinRush\wsl-distro.txt"

if not defined WSL_DISTRO (
    if exist "%DISTRO_CONFIG%" set /p "WSL_DISTRO="<"%DISTRO_CONFIG%"
)

if not defined WSL_DISTRO (
    echo Installed WSL distributions:
    wsl.exe --list --quiet
    set /p "WSL_DISTRO=Enter a distro with Cyclone DDS 0.10.5: "
)

if not defined WSL_DISTRO (
    echo No WSL distro was selected.
    goto :fail
)

if not defined GAME_ROLE (
    set /p "GAME_ROLE=Start server or client? [server/client]: "
)

set "ROLE_OK="
if /i "%GAME_ROLE%"=="server" set "ROLE_OK=1"
if /i "%GAME_ROLE%"=="client" set "ROLE_OK=1"
if /i "%GAME_ROLE%"=="invite-client2" set "ROLE_OK=1"
if /i "%GAME_ROLE%"=="invite-client3" set "ROLE_OK=1"
if /i "%GAME_ROLE%"=="invite-client4" set "ROLE_OK=1"
if /i "%GAME_ROLE%"=="invite-client5" set "ROLE_OK=1"
if /i "%GAME_ROLE%"=="invite-client6" set "ROLE_OK=1"
if not defined ROLE_OK (
    echo Usage: run-game.bat [WSL-distro-name] [server^|client^|invite-client2..6]
    goto :fail
)

if not exist "%~dp0run-game-wsl.sh" (
    echo Cannot find "%~dp0run-game-wsl.sh".
    goto :fail
)

if defined LOCALAPPDATA (
    if not exist "%LOCALAPPDATA%\CoinRush" mkdir "%LOCALAPPDATA%\CoinRush" >nul 2>nul
    >"%DISTRO_CONFIG%" echo %WSL_DISTRO%
)

echo Starting Coin Rush %GAME_ROLE% in WSL distro "%WSL_DISTRO%"...
rem WSL distro names have no spaces; pass the registered name as one unquoted token.
rem This mirrors: wsl --distribution Ubuntu-26.04 --exec bash run-game-wsl.sh server
wsl.exe --distribution %WSL_DISTRO% --exec bash run-game-wsl.sh %GAME_ROLE%
if errorlevel 1 goto :fail
exit /b 0

:fail
echo.
echo The launcher stopped. Check the error message above.
if not defined COINRUSH_NO_PAUSE pause
exit /b 1
