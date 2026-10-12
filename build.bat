@echo off
rem Compila NeXo 2 con un cmake que sepa usar Visual Studio 2022, aunque en el PATH haya
rem otro cmake delante (por ejemplo el de devkitPro/MSYS2, que solo genera "Unix Makefiles").
rem   build.bat          -> configurar + compilar (Release)
rem   build.bat test     -> ademas, ejecutar nexo2_tests
rem   Tambien compila NeXo2-Qt.exe: si no hay Qt 6, lo descarga en externals\qt (con Python)
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
rem Qt 6 para NeXo2-Qt.exe: NEXO2_QT_DIR, externals\qt (lo descarga este script) o C:\Qt\6.x
rem NEXO2_NO_QT=1: no descargar Qt (solo se compila NeXo2.exe)
set "QT_VERSION=6.8.3"
set "QTARG="
if not defined NEXO2_QT_DIR if exist "externals\qt\%QT_VERSION%\msvc2022_64\bin\windeployqt.exe" set "NEXO2_QT_DIR=%~dp0externals\qt\%QT_VERSION%\msvc2022_64"
if not defined NEXO2_QT_DIR (
    for /d %%d in ("C:\Qt\6.*") do if exist "%%d\msvc2022_64\bin\windeployqt.exe" set "NEXO2_QT_DIR=%%d\msvc2022_64"
)
if not defined NEXO2_QT_DIR if not "%NEXO2_NO_QT%"=="1" call :get_qt
if defined NEXO2_QT_DIR (
    echo Qt: "%NEXO2_QT_DIR%"
    set "QTARG=-DCMAKE_PREFIX_PATH=%NEXO2_QT_DIR%"
) else (
    echo Qt no encontrado: solo se compila NeXo2.exe. Ver docs\07-nexo-internals\frontend-qt.md
)
"%CMAKE%" -B build -G "Visual Studio 17 2022" %QTARG% || exit /b 1
"%CMAKE%" --build build --config Release || exit /b 1
if /i "%~1"=="test" build\Release\nexo2_tests.exe
endlocal
exit /b 0

rem Descarga Qt (solo lo necesario, ~600 MB) en externals\qt con aqtinstall. Necesita Python.
rem externals\qt no se sube al repositorio (.gitignore).
:get_qt
set "PY="
where py >nul 2>nul && set "PY=py"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY (
    echo Qt no esta y no encuentro Python para descargarlo: solo se compila NeXo2.exe
    exit /b 0
)
echo Descargando Qt %QT_VERSION% en externals\qt (solo la primera vez, tarda unos minutos)...
%PY% -m pip install --quiet --user aqtinstall
%PY% -m aqt install-qt windows desktop %QT_VERSION% win64_msvc2022_64 -O "%~dp0externals\qt"
if exist "externals\qt\%QT_VERSION%\msvc2022_64\bin\windeployqt.exe" set "NEXO2_QT_DIR=%~dp0externals\qt\%QT_VERSION%\msvc2022_64"
exit /b 0
