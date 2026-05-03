@echo off
setlocal

set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%"

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    set VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe
)

set MSBUILD=
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe 2^>nul`) do (
        set MSBUILD=%%i
        goto :found_msbuild
    )
)

:found_msbuild
if not defined MSBUILD (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" (
        set MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" (
        set MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" (
        set MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe
    ) else (
        echo [ERROR] MSBuild not found.
        exit /b 1
    )
)

taskkill /F /IM app.exe >nul 2>&1

echo [BUILD] Shader.sln Release x64
"%MSBUILD%" "%SCRIPT_DIR%Shader.sln" /p:Configuration=Release /p:Platform=x64 /m /v:minimal
if errorlevel 1 (
    echo.
    echo [FAIL] Build failed
    exit /b 1
)

if not exist "%SCRIPT_DIR%bin" mkdir "%SCRIPT_DIR%bin"

copy /y "%SCRIPT_DIR%assets\Shaders\scene.hlsl" "%SCRIPT_DIR%bin\" >nul 2>&1
copy /y "%SCRIPT_DIR%assets\Shaders\water.hlsl" "%SCRIPT_DIR%bin\" >nul 2>&1
copy /y "%SCRIPT_DIR%assets\Shaders\post.hlsl" "%SCRIPT_DIR%bin\" >nul 2>&1
copy /y "%SCRIPT_DIR%assets\Shaders\skybake.hlsl" "%SCRIPT_DIR%bin\" >nul 2>&1
copy /y "%SCRIPT_DIR%assets\Shaders\skybox.hlsl" "%SCRIPT_DIR%bin\" >nul 2>&1
copy /y "%SCRIPT_DIR%assets\Shaders\taa.hlsl" "%SCRIPT_DIR%bin\" >nul 2>&1
copy /y "%SCRIPT_DIR%assets\Shaders\foam.hlsl" "%SCRIPT_DIR%bin\" >nul 2>&1
copy /y "%SCRIPT_DIR%assets\Textures\noise.png" "%SCRIPT_DIR%bin\" >nul 2>&1

echo.
echo [OK] Build success: bin\app.exe
endlocal
