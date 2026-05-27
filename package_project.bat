@echo on
setlocal

set UAT=E:\Ue5_engine\UE_5.5\Engine\Build\BatchFiles\RunUAT.bat
set PROJECT_FILE=E:\UE_project\demo_createUE_exe\dev_uedemo\dev_uedemo.uproject
set OUTPUT_DIR=E:\UE_project\Release\RenderPlatform_v1.0
set LOG_FILE=%OUTPUT_DIR%\build_log.txt

echo Starting BuildCookRun...

if not exist "%UAT%" (
    echo UAT not found
    pause
    exit /b 1
)

if not exist "%PROJECT_FILE%" (
    echo Project not found
    pause
    exit /b 1
)

if exist "%OUTPUT_DIR%" rmdir /s /q "%OUTPUT_DIR%"
mkdir "%OUTPUT_DIR%"

call "%UAT%" BuildCookRun ^
-project="%PROJECT_FILE%" ^
-noP4 ^
-platform=Win64 ^
-clientconfig=Development ^
-build ^
-cook ^
-stage ^
-archive ^
-archivedirectory="%OUTPUT_DIR%" ^
-NoPrecompiledEngine ^
-log ^
> "%LOG_FILE%" 2>&1

echo ERRORLEVEL=%ERRORLEVEL%
pause
endlocal