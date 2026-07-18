@echo off
REM Start notes-server (FastAPI backend)
REM Prerequisite: copy .env.example to .env and fill your API keys
SET "PY=D:\VibeApp\py\python.exe"
IF NOT EXIST "%PY%" (
    echo [ERROR] Python not found at D:\VibeApp\py\python.exe
    echo Please edit this bat and set PY to your python path.
    pause
    exit /b 1
)
IF NOT EXIST ".env" (
    echo [INFO] No .env found, using defaults or env vars.
    echo         Copy .env.example to .env for custom config.
)
cd /d %~dp0
"%PY%" -m uvicorn main:app --host 127.0.0.1 --port 8000 --reload
pause
