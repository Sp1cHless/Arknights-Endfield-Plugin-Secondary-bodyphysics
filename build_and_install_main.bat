@echo off
rem ============================================================
rem Local verification entry point. Build the current runtime and
rem framework-dependent Manager, refresh the source tool folder,
rem then install the runtime DLLs into the local game.
rem Run this before publish_release.bat.
rem ============================================================
setlocal
set "ROOT=%~dp0"
set "GAMEROOT=E:\GAMU\Hypergryph Launcher\games\Endfield Game"
if not "%~1"=="" set "GAMEROOT=%~1"
set "GAMEPLUGIN=%GAMEROOT%\plugin"
set "DOTNET=%ProgramFiles%\dotnet\dotnet.exe"
if not exist "%DOTNET%" set "DOTNET=%LOCALAPPDATA%\Microsoft\dotnet\dotnet.exe"
set "MANAGER_OUT=%ROOT%Manager\bin\Release\net8.0-windows"
set "MANAGER_FILES=SecondaryMotion.Manager.exe SecondaryMotion.Manager.dll SecondaryMotion.Manager.pdb SecondaryMotion.Manager.deps.json SecondaryMotion.Manager.runtimeconfig.json Wpf.Ui.dll"

tasklist /fo csv | findstr /i /c:"Endfield.exe" >nul
if not errorlevel 1 (
    echo [ERROR] Close Endfield.exe before install.
    pause
    exit /b 1
)
tasklist /fo csv | findstr /i /c:"SecondaryMotion.Manager.exe" >nul
if not errorlevel 1 (
    echo [ERROR] Close SecondaryMotion.Manager.exe before install.
    pause
    exit /b 1
)
if not exist "%GAMEROOT%\Endfield.exe" if not exist "%GAMEROOT%\UnityPlayer.dll" (
    echo [ERROR] Game root marker not found: %GAMEROOT%
    pause
    exit /b 1
)

pushd "%ROOT%"
echo [1/6] Building current runtime source...
call build.bat <nul
if errorlevel 1 (
    echo [ERROR] Runtime build failed.
    popd
    pause
    exit /b 1
)

if not exist "%DOTNET%" (
    echo [ERROR] dotnet SDK not found: %DOTNET%
    popd
    pause
    exit /b 1
)

echo [2/6] Building current Manager source...
"%DOTNET%" build "%ROOT%Manager\SecondaryMotion.Manager.csproj" -c Release
if errorlevel 1 (
    echo [ERROR] Manager build failed.
    popd
    pause
    exit /b 1
)

for %%F in (sbm.dll d3dcompiler_47.dll vulkan-1.dll) do (
    if not exist "bin\%%F" (
        echo [ERROR] Missing bin\%%F
        popd
        pause
        exit /b 1
    )
)

for %%F in (%MANAGER_FILES%) do (
    if not exist "%MANAGER_OUT%\%%F" (
        echo [ERROR] Missing Manager output: %%F
        popd
        pause
        exit /b 1
    )
)

echo [3/6] Refreshing the source tool plugin folder...
if not exist "SecondaryMotion\plugin" mkdir "SecondaryMotion\plugin"
copy /y "bin\sbm.dll" "SecondaryMotion\plugin\sbm.dll" >nul
copy /y "bin\d3dcompiler_47.dll" "SecondaryMotion\plugin\d3dcompiler_47.dll" >nul
copy /y "bin\vulkan-1.dll" "SecondaryMotion\plugin\vulkan-1.dll" >nul

echo [4/6] Refreshing the source tool Manager...
for %%F in (%MANAGER_FILES%) do copy /y "%MANAGER_OUT%\%%F" "SecondaryMotion\%%F" >nul

echo [5/6] Backing up and installing game DLLs...
if not exist "%GAMEPLUGIN%" mkdir "%GAMEPLUGIN%"
if exist "%GAMEPLUGIN%\sbm.dll" copy /y "%GAMEPLUGIN%\sbm.dll" "%GAMEPLUGIN%\sbm.dll.pre_source_install" >nul
if exist "%GAMEROOT%\d3dcompiler_47.dll" copy /y "%GAMEROOT%\d3dcompiler_47.dll" "%GAMEROOT%\d3dcompiler_47.dll.pre_source_install" >nul
if exist "%GAMEROOT%\vulkan-1.dll" copy /y "%GAMEROOT%\vulkan-1.dll" "%GAMEROOT%\vulkan-1.dll.pre_source_install" >nul
copy /y "bin\sbm.dll" "%GAMEPLUGIN%\sbm.dll" >nul
copy /y "bin\d3dcompiler_47.dll" "%GAMEROOT%\d3dcompiler_47.dll" >nul
copy /y "bin\vulkan-1.dll" "%GAMEROOT%\vulkan-1.dll" >nul

echo [6/6] Verifying...
fc /b "bin\sbm.dll" "%GAMEPLUGIN%\sbm.dll" >nul
if errorlevel 1 goto verify_fail
fc /b "bin\d3dcompiler_47.dll" "%GAMEROOT%\d3dcompiler_47.dll" >nul
if errorlevel 1 goto verify_fail
fc /b "bin\vulkan-1.dll" "%GAMEROOT%\vulkan-1.dll" >nul
if errorlevel 1 goto verify_fail
for %%F in (%MANAGER_FILES%) do (
    fc /b "%MANAGER_OUT%\%%F" "SecondaryMotion\%%F" >nul
    if errorlevel 1 goto verify_fail
)
popd
echo.
echo INSTALL=PASS
echo Current runtime is active in the game folder.
echo Current Manager is active in the source tool folder.
pause
exit /b 0

:verify_fail
popd
echo [ERROR] Installed file verification failed.
pause
exit /b 1
