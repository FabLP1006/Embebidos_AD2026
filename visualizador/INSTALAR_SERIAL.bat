@echo off
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 -m pip install pyserial
) else (
    python -m pip install pyserial
)
pause
