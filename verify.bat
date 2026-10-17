@echo off
rem verify.bat - V2 automated verification. Run from project root.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"

echo === [1/5] Unit tests ===
cl /nologo /utf-8 /O2 /EHsc /std:c++17 verify_tests.cpp /Fe:verify_tests.exe > verify_build.log 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] verify_tests.cpp build failed
    type verify_build.log
    exit /b 1
)
verify_tests.exe
if %errorlevel% neq 0 (
    echo [VERIFY] unit tests FAILED
    exit /b 1
)
python verify_ui_serialization.py
if %errorlevel% neq 0 (
    echo [VERIFY] UI/serialization checks FAILED
    exit /b 1
)

echo.
echo === [2/5] Manager official character migration tests ===
dotnet run --project tests\OfficialCharacterMigrationTests.csproj -c Release -- "%CD%" "%CD%\artifacts\official-character-tests"
if %errorlevel% neq 0 (
    echo [VERIFY] Manager migration tests FAILED
    exit /b 1
)

echo.
echo === [3/5] Forbidden hardcode scan (strict reflection) ===
set "VIOL=0"
findstr /s /n /c:"+ 0x" src\*.h src\*.cpp > nul && echo   VIOLATION: fixed offset addition in code && set "VIOL=1"
findstr /s /n /c:"gaMod +" src\*.h src\*.cpp > nul && echo   VIOLATION: RVA addition in code && set "VIOL=1"
findstr /s /n /c:"fallback 0x" src\*.h src\*.cpp > nul && echo   VIOLATION: fixed offset fallback && set "VIOL=1"
if "%VIOL%"=="0" (
    echo   PASS: no forbidden hardcode
) else (
    echo   [VERIFY] forbidden hardcode scan FAILED
    exit /b 1
)

echo.
echo === [4/5] JSON syntax sanity ===
python -c "import json; [json.load(open(f,encoding='utf-8')) for f in ['SecondaryMotion/data/characters.default.json','SecondaryMotion/presets/Default.json','SecondaryMotion/presets/User.json','SecondaryMotion/runtime/config.json']]; print('  PASS: all JSON valid')" 2>nul
if %errorlevel% neq 0 (
    echo   [VERIFY] JSON syntax check FAILED
    exit /b 1
)

echo.
echo === [5/5] Full Runtime build ===
call build.bat
if %errorlevel% neq 0 (
    echo   [VERIFY] Runtime build FAILED
    exit /b 1
)

echo.
echo ==========================================
echo   VERIFICATION: PASS
echo ==========================================
