@echo off
rem ===========================================================================
rem  Provision LLM Engine launcher -- starts provision-server in router mode with the bundled
rem  models.ini.  This is the Windows equivalent of docker-dual/entrypoint.sh.
rem  Closing this console window stops the server.
rem ===========================================================================
setlocal

rem CWD = this folder, so the relative paths in models.ini resolve correctly.
cd /d "%~dp0"

rem --- knobs (override by editing here) --------------------------------------
rem PORT         : HTTP port for API + WebUI
rem HOST         : 127.0.0.1 = local only.  Set 0.0.0.0 to expose on the LAN
rem                (then also allow provision-server.exe through Windows Firewall).
rem MODELS_MAX   : 2 = keep chat + embed resident (fits 11 GB GTX 1080 Ti)
rem                1 = swap between requests (~+1-5 s on switch, less VRAM)
set "PORT=8080"
set "HOST=127.0.0.1"
set "MODELS_MAX=2"
set "MODELS_CONFIG=models.ini"
rem --------------------------------------------------------------------------

if not exist "provision-server.exe" (
    echo [launcher] ERROR: provision-server.exe not found next to this script.
    echo [launcher] The installation looks incomplete -- reinstall Provision LLM Engine.
    pause
    exit /b 1
)
if not exist "%MODELS_CONFIG%" (
    echo [launcher] ERROR: %MODELS_CONFIG% not found.
    pause
    exit /b 1
)

title Provision LLM Engine  -  http://%HOST%:%PORT%/
echo ===========================================================================
echo  Provision LLM Engine (provision-server router)
echo  WebUI / API : http://%HOST%:%PORT%/
echo  Chat model  : qwen2.5-chat
echo  Embed model : gte-qwen2-embed
echo  MODELS_MAX  : %MODELS_MAX%   (edit ProvisionLLM_launcher.bat to change)
echo  Stop        : close this window or press Ctrl+C
echo ===========================================================================
echo.

provision-server.exe ^
    --host %HOST% ^
    --port %PORT% ^
    --models-preset "%MODELS_CONFIG%" ^
    --models-max %MODELS_MAX%

echo.
echo [launcher] provision-server exited (code %ERRORLEVEL%). Press any key to close.
pause >nul
endlocal
