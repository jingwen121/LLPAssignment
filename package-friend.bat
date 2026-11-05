@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PLAYER=%~1"
if not defined PLAYER set /p "PLAYER=Package for which identity? [client2-client6]: "

set "PLAYER_OK="
if /i "%PLAYER%"=="client2" (
    set "PLAYER=client2"
    set "PLAYER_OK=1"
)
if /i "%PLAYER%"=="client3" (
    set "PLAYER=client3"
    set "PLAYER_OK=1"
)
if /i "%PLAYER%"=="client4" (
    set "PLAYER=client4"
    set "PLAYER_OK=1"
)
if /i "%PLAYER%"=="client5" (
    set "PLAYER=client5"
    set "PLAYER_OK=1"
)
if /i "%PLAYER%"=="client6" (
    set "PLAYER=client6"
    set "PLAYER_OK=1"
)
if not defined PLAYER_OK (
    echo Choose an unused identity: client2, client3, client4, client5, or client6.
    goto :fail
)

if exist "invites\coinrush-%PLAYER%.tar.gz.gpg" (
    echo An invite already exists for %PLAYER%. Move it aside and retry, or choose another identity.
    goto :fail
)

echo When prompted for the invite passphrase, enter coinrushgame twice.
echo The passphrase will not be included in the package.
call "%~dp0create-invite.bat" "%PLAYER%" "%~2"
if errorlevel 1 goto :fail

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\package-friend.ps1" -Player "%PLAYER%"
if errorlevel 1 goto :fail

echo.
echo Package created in packages\CoinRush-%PLAYER%.zip
echo Send the zip to this friend and give them the passphrase separately.
pause
exit /b 0

:fail
echo.
echo Package creation failed. Review the message above.
pause
exit /b 1
