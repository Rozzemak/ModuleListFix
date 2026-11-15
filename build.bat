@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>&1
if errorlevel 1 (
    echo Error: cl.exe not found. Run this from an "x64 Native Tools Command Prompt for VS".
    exit /b 1
)
cl /nologo /LD /EHsc /O2 /Fe:ModuleListFix.dll src\ModuleListFix.cpp /link /MACHINE:X64
if errorlevel 1 exit /b 1
echo Built ModuleListFix.dll
