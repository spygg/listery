@echo off
setlocal

set "ROOT=%~dp0"

if "%QTDIR%"=="" set "QTDIR=E:\programes\Qt\Qt\5.15.2\mingw81_64"
if "%MINGW%"=="" set "MINGW=E:\programes\Qt\Qt\Tools\mingw810_64"
if "%NINJA_DIR%"=="" set "NINJA_DIR=E:\programes\Qt\Qt\Tools\Ninja"
if "%CMAKE_EXE%"=="" set "CMAKE_EXE=E:\programes\Qt\Qt\Tools\CMake_64\bin\cmake.exe"

if not exist "%CMAKE_EXE%" (
    echo [ERROR] cmake not found: %CMAKE_EXE%
    exit /b 1
)

set "PATH=%MINGW%\bin;%NINJA_DIR%;%QTDIR%\bin;%PATH%"

"%CMAKE_EXE%" -S "%ROOT%." -B "%ROOT%build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QTDIR%
if errorlevel 1 exit /b 1

"%CMAKE_EXE%" --build "%ROOT%build" --parallel
if errorlevel 1 exit /b 1

echo.
echo [OK] %ROOT%build\listery.exe
endlocal