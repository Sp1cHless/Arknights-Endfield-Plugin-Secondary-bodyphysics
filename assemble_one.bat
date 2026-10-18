@echo off
rem ============================================================
rem assemble_one.bat - assemble one language package from the
rem shared publish output (dist\staging_app) and zip it.
rem Usage: assemble_one.bat <LANG> <VERSION>
rem   LANG = EN or ZH (zip filename + default_lang.txt inside)
rem ============================================================
setlocal

set "LANG=%~1"
set "VER=%~2"
set "ROOT=%~dp0"
set "DIST=%ROOT%dist"
set "APP=%DIST%\staging_app"
set "STAGE=%DIST%\staging_%LANG%\SecondaryMotion"
set "RUNTIME_DIR=runtime"

if exist "%DIST%\staging_%LANG%" rmdir /s /q "%DIST%\staging_%LANG%"
mkdir "%STAGE%"

echo   Copying Manager build (%LANG%) ...
copy /y "%APP%\*" "%STAGE%\" >nul || goto :assemble_failed

echo   Assembling %LANG% package ...
mkdir "%STAGE%\plugin"
mkdir "%STAGE%\data"
mkdir "%STAGE%\presets"
mkdir "%STAGE%\%RUNTIME_DIR%"
copy /y "%ROOT%bin\sbm.dll" "%STAGE%\plugin\" >nul || goto :assemble_failed
copy /y "%ROOT%bin\d3dcompiler_47.dll" "%STAGE%\plugin\" >nul || goto :assemble_failed
copy /y "%ROOT%bin\vulkan-1.dll" "%STAGE%\plugin\" >nul || goto :assemble_failed
copy /y "%ROOT%SecondaryMotion\data\characters.default.json" "%STAGE%\data\characters.default.template.json" >nul || goto :assemble_failed
copy /y "%ROOT%SecondaryMotion\presets\Default.json" "%STAGE%\presets\Default.template.json" >nul || goto :assemble_failed
copy /y "%ROOT%SecondaryMotion\presets\Default.v3.0.0-bad.template.json" "%STAGE%\presets\Default.v3.0.0-bad.template.json" >nul || goto :assemble_failed
copy /y "%ROOT%SecondaryMotion\presets\User.json" "%STAGE%\presets\User.template.json" >nul || goto :assemble_failed
copy /y "%ROOT%SecondaryMotion\%RUNTIME_DIR%\config.json" "%STAGE%\%RUNTIME_DIR%\" >nul || goto :assemble_failed
copy /y "%ROOT%USER_GUIDE_EN.txt" "%STAGE%\" >nul || goto :assemble_failed
copy /y "%ROOT%README.md" "%STAGE%\" >nul || goto :assemble_failed
rem language default for this package
if /i "%LANG%"=="EN" (
    echo en-US> "%STAGE%\default_lang.txt"
) else (
    echo zh-CN> "%STAGE%\default_lang.txt"
)
rem ZH package: localize character display names in the DB copy and
rem add the Chinese user guide (zh_names.py handles both; stage arg = 2nd)
if /i "%LANG%"=="ZH" (
    python "%ROOT%zh_names.py" "%STAGE%\data\characters.default.template.json" "%STAGE%" || goto :assemble_failed
)
rem drop dev-only artifacts that publish may have produced
del /q "%STAGE%\*.pdb" 2>nul
del /q "%STAGE%\settings.json" 2>nul

echo   Zipping %LANG% ...
python "%ROOT%tools\package_release.py" "%STAGE%" "%DIST%\ShakingBreastManager-%VER%-%LANG%-win-x64.zip"
if errorlevel 1 ( echo [ERROR] zip %LANG% failed & exit /b 1 )

echo   Package: %DIST%\ShakingBreastManager-%VER%-%LANG%-win-x64.zip
exit /b 0

:assemble_failed
echo [ERROR] package assembly failed for %LANG%
exit /b 1
