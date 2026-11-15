@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>&1
if errorlevel 1 (
    echo Error: cl.exe not found.
    echo Open the Native Tools Command Prompt matching the target architecture
    echo x64 Native Tools for the 64-bit DLL, x86 Native Tools for the 32-bit one.
    exit /b 1
)
cl /nologo /LD /EHsc /O2 /Fe:ModuleListFix.dll src\ModuleListFix.cpp
if errorlevel 1 exit /b 1
echo Built ModuleListFix.dll
