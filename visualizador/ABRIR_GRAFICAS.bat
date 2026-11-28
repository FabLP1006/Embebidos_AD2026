@echo off
cd /d "%~dp0"
rem Usa el lanzador "py" (Python 3 por defecto, el que tiene pyserial); si no existe, usa "python".
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 myogrip_monitor.py
) else (
    python myogrip_monitor.py
)
if errorlevel 1 pause
