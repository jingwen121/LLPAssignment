@echo off
setlocal
set "COINRUSH_NO_PAUSE=1"
call "%~dp0run-game.bat" "%~1" server
set "RUN_RESULT=%ERRORLEVEL%"
echo.
if not "%RUN_RESULT%"=="0" echo Server launcher exited with error %RUN_RESULT%.
if "%RUN_RESULT%"=="0" echo Server process has stopped.
pause
exit /b %RUN_RESULT%
