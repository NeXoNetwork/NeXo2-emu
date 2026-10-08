@echo off
rem Compila NeXo 2 con un cmake que sepa usar Visual Studio 2022, aunque en el PATH haya
rem otro cmake delante (por ejemplo el de devkitPro/MSYS2, que solo genera "Unix Makefiles").
rem   build.bat          -> configurar + compilar (Release)
rem   build.bat test     -> ademas, ejecutar nexo2_tests
setlocal
set "CMAKE="
rem 1. CMake instalado aparte (cmake.org)
if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE=%ProgramFiles%\CMake\bin\cmake.exe"
if not defined CMAKE if exist "%ProgramFiles(x86)%\CMake\bin\cmake.exe" set "CMAKE=%ProgramFiles(x86)%\CMake\bin\cmake.exe"
rem 2. El que trae Visual Studio
if not defined CMAKE (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
        for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do (
            if exist "%%i\Common7\IDE\CommonExtensionTools\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=%%i\Common7\IDE\CommonExtensionTools\Microsoft\CMake\CMake\bin\cmake.exe"
        )
    )
)
rem 3. Cualquier cmake del PATH que no sea el de MSYS2/devkitPro
if not defined CMAKE (
    for /f "delims=" %%i in ('where cmake 2^>nul') do (
        if not defined CMAKE (
            echo %%i | findstr /i "msys devkitpro" >nul || set "CMAKE=%%i"
        )
    )
)
if not defined CMAKE (
    echo No encuentro un cmake para Visual Studio. Instala CMake desde https://cmake.org/download/
    exit /b 1
)
echo Usando "%CMAKE%"
cd /d "%~dp0"
"%CMAKE%" -B build -G "Visual Studio 17 2022" || exit /b 1
"%CMAKE%" --build build --config Release || exit /b 1
if /i "%~1"=="test" build\Release\nexo2_tests.exe
endlocal
