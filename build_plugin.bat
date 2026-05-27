@echo on
setlocal

set UAT=E:\Ue5_engine\UE_5.5\Engine\Build\BatchFiles\RunUAT.bat
set PLUGIN_FILE=E:\UE_project\demo_createUE_exe\dev_uedemo\Plugins\RenderCoreExt\RenderCoreExt.uplugin
set OUTPUT_DIR=E:\UE_project\Release\RenderCoreExt_Plugin_v1.0
set LOG_FILE=%OUTPUT_DIR%\build_log.txt

echo ============================================================
echo Building RenderCoreExt Plugin
echo ============================================================

if exist "%OUTPUT_DIR%" (
    echo [INFO] Cleaning old output...
    rmdir /s /q "%OUTPUT_DIR%"
)

mkdir "%OUTPUT_DIR%"

echo [INFO] Running UAT...

call "%UAT%" BuildPlugin ^
-Plugin="%PLUGIN_FILE%" ^
-Package="%OUTPUT_DIR%" ^
-Rocket ^
-TargetPlatforms=Win64 ^
> "%LOG_FILE%" 2>&1

echo.
echo ============================================================
echo Build finished. ErrorLevel = %ERRORLEVEL%
echo Log saved to: %LOG_FILE%
echo ============================================================

pause
cmd /k
endlocal