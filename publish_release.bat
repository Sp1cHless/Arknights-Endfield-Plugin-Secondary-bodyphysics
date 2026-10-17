@echo off
rem ============================================================
rem publish_release.bat - build runtime, verify, publish the SINGLE
rem Manager once, then assemble EN + ZH language packages.
rem Usage: publish_release.bat [version-tag]   (default: Manager project version)
rem ============================================================
setlocal

set "VER=%~1"
set "ROOT=%~dp0"
set "DIST=%ROOT%dist"
set "DOTNET=%ProgramFiles%\dotnet\dotnet.exe"
if not exist "%DOTNET%" set "DOTNET=%LOCALAPPDATA%\Microsoft\dotnet\dotnet.exe"
rem Package names must match the built program, regardless of older local zips.
set "PROJECT_VER="
for /f %%v in ('powershell -NoProfile -Command "$p=[xml](Get-Content -LiteralPath '%ROOT%Manager\SecondaryMotion.Manager.csproj' -Raw); 'v'+$p.SelectSingleNode('/Project/PropertyGroup/Version').InnerText"') do set "PROJECT_VER=%%v"
if "%PROJECT_VER%"=="" ( echo [ERROR] Could not read Manager version & exit /b 1 )
if "%VER%"=="" set "VER=%PROJECT_VER%"
if not "%VER%"=="%PROJECT_VER%" ( echo [ERROR] Package version must match Manager version %PROJECT_VER% & exit /b 1 )
echo Version: %VER%

echo [1/5] Building runtime (sbm.dll + proxies) ...
call "%ROOT%build.bat" <nul
if errorlevel 1 ( echo [ERROR] build.bat failed & exit /b 1 )

echo [2/5] Verifying ...
call "%ROOT%verify.bat" <nul
if errorlevel 1 ( echo [ERROR] verify.bat failed & exit /b 1 )

echo [3/5] Publishing Manager (single build, self-contained win-x64) ...
if exist "%DIST%\staging_app" rmdir /s /q "%DIST%\staging_app"
mkdir "%DIST%\staging_app"
"%DOTNET%" publish "%ROOT%Manager\SecondaryMotion.Manager.csproj" -c Release -r win-x64 --self-contained true -o "%DIST%\staging_app"
if errorlevel 1 ( echo [ERROR] dotnet publish failed & exit /b 1 )

echo [4/5] English package ...
call "%ROOT%assemble_one.bat" EN "%VER%"
if errorlevel 1 ( echo [ERROR] EN package failed & exit /b 1 )

echo [5/5] Chinese package ...
call "%ROOT%assemble_one.bat" ZH "%VER%"
if errorlevel 1 ( echo [ERROR] ZH package failed & exit /b 1 )

echo %VER%> "%DIST%\last_version.txt"
echo.
echo ==========================================
for %%Z in ("%DIST%\ShakingBreastManager-%VER%-EN-win-x64.zip" "%DIST%\ShakingBreastManager-%VER%-ZH-win-x64.zip") do (
    echo   %%~nZ%%~xZ
    certutil -hashfile "%%Z" SHA256 | findstr /v "hash"
)
echo ==========================================
echo Done.
pause
