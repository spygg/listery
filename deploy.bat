@echo off
setlocal

set "ROOT=%~dp0"
set "DIST=%ROOT%dist"
if "%~1"=="" (set "BUILD_DIR=%ROOT%build") else (set "BUILD_DIR=%~1")

if "%QTDIR%"=="" set "QTDIR=E:\programes\Qt\Qt\5.15.2\mingw81_64"
if "%MINGW%"=="" set "MINGW=E:\programes\Qt\Qt\Tools\mingw810_64"
if "%QT_MAJOR%"=="" set "QT_MAJOR=5"

if not exist "%BUILD_DIR%\listery.exe" (
    echo [ERROR] %BUILD_DIR%\listery.exe not found, run build.bat first
    exit /b 1
)
if not exist "%QTDIR%\plugins\platforms\qwindows.dll" (
    echo [ERROR] Qt platform plugin not found: %QTDIR%\plugins\platforms
    exit /b 1
)

if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%\platforms" "%DIST%\sqldrivers" "%DIST%\styles"

copy /y "%BUILD_DIR%\listery.exe" "%DIST%\" >nul

for %%m in (Core Gui Widgets Sql Network) do (
    if exist "%QTDIR%\bin\Qt%QT_MAJOR%%%m.dll" copy /y "%QTDIR%\bin\Qt%QT_MAJOR%%%m.dll" "%DIST%\" >nul
)
for %%f in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do (
    if exist "%MINGW%\bin\%%f" copy /y "%MINGW%\bin\%%f" "%DIST%\" >nul
)

copy /y "%QTDIR%\plugins\platforms\qwindows.dll" "%DIST%\platforms\" >nul
copy /y "%QTDIR%\plugins\sqldrivers\qsqlite.dll" "%DIST%\sqldrivers\" >nul
for %%f in ("%QTDIR%\plugins\styles\*.dll") do copy /y "%%~f" "%DIST%\styles\" >nul

echo.
echo [OK] %DIST%\listery.exe
endlocal