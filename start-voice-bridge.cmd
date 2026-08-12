@echo off
cd /d "%~dp0"
if exist "dist-final\VoiceBridge\VoiceBridge.exe" (
  start "" "dist-final\VoiceBridge\VoiceBridge.exe"
  exit /b
)
python app.py
if errorlevel 1 pause
