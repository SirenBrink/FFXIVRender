@echo off
setlocal
if "%NGX_SDK_DIR%"=="" if exist "%~dp0..\.cache\ngx-sdk\nvsdk_ngx.h" set "NGX_SDK_DIR=%~dp0..\.cache\ngx-sdk"
if "%NGX_SDK_DIR%"=="" (
 echo Set NGX_SDK_DIR to the licensed NVIDIA SDK include directory.
 exit /b 1
)
where cl >nul 2>nul
if errorlevel 1 (
 for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
)
cmake -S "%~dp0.." -B "%~dp0..\build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNGX_SDK_DIR="%NGX_SDK_DIR%"
if not "%errorlevel%"=="0" exit /b 1
cmake --build "%~dp0..\build" --parallel 4
if not "%errorlevel%"=="0" exit /b 1
set TEMP=%~dp0..\build\test-temp
set TMP=%TEMP%
if not exist "%TEMP%" mkdir "%TEMP%"
ctest --test-dir "%~dp0..\build" --output-on-failure

if not "%errorlevel%"=="0" exit /b 1
cmake -E touch "%~dp0..\build\tests-passed.stamp"
