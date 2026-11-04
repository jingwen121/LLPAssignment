@echo off
setlocal
set "COINRUSH_NO_PAUSE=1"
call "%~dp0run-game.bat" "%~1" client
set "RUN_RESULT=%ERRORLEVEL%"
if not "%RUN_RESULT%"=="0" (
    echo.
    echo Client launcher exited with error %RUN_RESULT%.
    pause
)
exit /b %RUN_RESULT%
