@echo off
setlocal

set "PROJECT_ROOT=%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 10

set "VS_ROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%i"
if not defined VS_ROOT exit /b 11

call "%VS_ROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 12

cd /d "%PROJECT_ROOT%"
if not exist "build-shm-smoke" mkdir "build-shm-smoke"

cl /nologo /std:c++17 /EHsc /W4 /WX ^
  /Fo:"build-shm-smoke\\" ^
  /Fe:"build-shm-smoke\shared_memory_cpp_smoke.exe" ^
  "tests\shared_memory_cpp_smoke.cpp" ^
  "src\ipc\SharedMemoryRegion.cpp"
if errorlevel 1 exit /b 13

"build-shm-smoke\shared_memory_cpp_smoke.exe"
if errorlevel 1 exit /b 14

set "PYTHONDONTWRITEBYTECODE=1"
python "tests\run_shared_memory_cross_language.py" "build-shm-smoke\shared_memory_cpp_smoke.exe"
exit /b %errorlevel%
