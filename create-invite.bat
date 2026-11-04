@echo off
setlocal
set "PLAYER=%~1"
if not defined PLAYER set /p "PLAYER=Which client identity? [client2-client6]: "

if /i not "%PLAYER%"=="client2" if /i not "%PLAYER%"=="client3" if /i not "%PLAYER%"=="client4" if /i not "%PLAYER%"=="client5" if /i not "%PLAYER%"=="client6" (
    echo Choose client2, client3, client4, client5, or client6.
    pause
    exit /b 1
)
if /i "%PLAYER%"=="CLIENT2" set "PLAYER=client2"
if /i "%PLAYER%"=="CLIENT3" set "PLAYER=client3"
if /i "%PLAYER%"=="CLIENT4" set "PLAYER=client4"
if /i "%PLAYER%"=="CLIENT5" set "PLAYER=client5"
if /i "%PLAYER%"=="CLIENT6" set "PLAYER=client6"

call "%~dp0scripts\run-game.bat" "%~2" "invite-%PLAYER%"
exit /b %ERRORLEVEL%
