@echo off
rem ============================================================
rem publish_release.bat - build runtime, verify, publish the SINGLE
rem Manager once, then assemble EN + ZH language packages.
rem Usage: publish_release.bat [version-tag]   (default auto patch+1)
rem ============================================================
setlocal

set "VER=%~1"
set "ROOT=%~dp0"
set "DIST=%ROOT%dist"
set "DOTNET=%ProgramFiles%\dotnet\dotnet.exe"
if not exist "%DOTNET%" set "DOTNET=%LOCALAPPDATA%\Microsoft\dotnet\dotnet.exe"
if "%VER%"=="" (
    rem auto-increment patch: read dist\last_version.txt, bump the 3rd digit
    for /f %%v in ('powershell -NoProfile -Command "$l=Get-Content '%DIST%\last_version.txt' -EA SilentlyContinue; if(-not $l){$l='v2.1.0'}; $m=[regex]::Match($l,'^v(\d+)\.(\d+)\.(\d+)'); if(-not $m.Success){'v2.1.0'} else { 'v{0}.{1}.{2}' -f $m.Groups[1].Value,$m.Groups[2].Value,([int]$m.Groups[3].Value+1) }"') do set "VER=%%v"
)
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
