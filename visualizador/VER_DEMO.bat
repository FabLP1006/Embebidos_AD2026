@echo off
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 myogrip_monitor.py --demo
) else (
    python myogrip_monitor.py --demo
)
if errorlevel 1 pause
