@echo off
rem PhoneLink Studio launcher: sets up Python on first run, then opens the control panel.
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
    echo First run: installing PhoneLink Studio ^(about 150 MB^)...
    where uv >nul 2>nul || powershell -ExecutionPolicy Bypass -NoProfile -Command "irm https://astral.sh/uv/install.ps1 | iex"
    set "PATH=%USERPROFILE%\.local\bin;%PATH%"
    uv venv --python 3.11 .venv || goto :fail
    uv pip install --python .venv\Scripts\python.exe -r requirements.txt || goto :fail
)
.venv\Scripts\python.exe app.py
exit /b
:fail
echo Setup failed - see the messages above.
pause
