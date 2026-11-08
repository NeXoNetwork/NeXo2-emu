@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem Compila NeXo 2 en Windows con Visual Studio 2022 y MSVC v143.
rem   build.bat          -> configurar + compilar (Release)
rem   build.bat test     -> ademas, ejecutar nexo2_tests
rem   set NEXO2_NO_QT=1  -> no descargar Qt ni compilar NeXo2-Qt.exe
cd /d "%~dp0"

set "MISSING=0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_PATH="
set "CMAKE="
set "GIT_OK=0"
set "VS_GENERATOR="
set "VS_TOOLSET_ARG=-T v143"
set "SDK_PLATFORM=x64"
set "BUILD_DIR=build"
set "MIN_CMAKE_MAJOR=3"
set "MIN_CMAKE_MINOR=25"
set "MIN_CMAKE_VERSION=3.25"

echo.
echo === Comprobando requisitos para compilar NeXo 2 ===

rem Requisito: Visual Studio 2022 o 2026 con el compilador MSVC x64.
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
)
rem Some VS2026 installations place vswhere outside the usual x86 installer path;
rem fall back to the standard install roots if its component query returned nothing.
if not defined VS_PATH (
    for /d %%i in ("%ProgramFiles%\Microsoft Visual Studio\18\*") do if exist "%%i\VC\Tools\MSVC" set "VS_PATH=%%i"
)
if not defined VS_PATH (
    for /d %%i in ("%ProgramFiles(x86)%\Microsoft Visual Studio\2022\*") do if exist "%%i\VC\Tools\MSVC" set "VS_PATH=%%i"
)
if defined VS_PATH (
    for /f "tokens=4 delims=\" %%v in ("!VS_PATH!") do set "VS_MAJOR=%%v"
    if "!VS_MAJOR!"=="18" (
        set "VS_GENERATOR=Visual Studio 18 2026"
        set "VS_TOOLSET_ARG=-T v145"
        set "BUILD_DIR=build-vs2026"
        set "MIN_CMAKE_MAJOR=4"
        set "MIN_CMAKE_MINOR=2"
        set "MIN_CMAKE_VERSION=4.2"
    ) else (
        if "!VS_MAJOR!"=="2022" set "VS_GENERATOR=Visual Studio 17 2022"
    )
)
if not defined VS_GENERATOR (
    echo [FALTA] Visual Studio 2022 o 2026 con la carga de trabajo "Desarrollo para el escritorio con C++".
    echo         Asegurate de incluir las herramientas MSVC x64/x86 y un Windows 10/11 SDK en Visual Studio Installer.
        echo         Descarga: https://visualstudio.microsoft.com/downloads/
    set "MISSING=1"
) else (
    set "V143_FOUND=0"
    for /d %%t in ("!VS_PATH!\VC\Tools\MSVC\14.*") do (
        set "TOOLSET=%%~nxt"
        for /f "tokens=1,2 delims=." %%a in ("!TOOLSET!") do (
            if "%%a"=="14" if %%b GEQ 30 if exist "%%t\bin\Hostx64\x64\cl.exe" set "V143_FOUND=1"
        )
    )
    if "!V143_FOUND!"=="0" (
        echo [FALTA] Herramientas de compilacion MSVC x64/x86. Abre Visual Studio Installer ^> Modificar ^> Componentes individuales.
        echo         Instala las herramientas de compilacion x64/x86; las opciones ARM no sirven para este build.
        set "MISSING=1"
    ) else (
        echo [OK] Visual Studio y herramientas MSVC x64/x86: !VS_PATH!
    )
)

rem Requisito: CMake 3.25 para VS2022, o 4.2 para VS2026. Primero se busca el que incluye VS;
rem despues CMake independiente y por ultimo uno compatible del PATH.
if defined VS_PATH if exist "!VS_PATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=!VS_PATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE=%ProgramFiles%\CMake\bin\cmake.exe"
if not defined CMAKE if exist "%ProgramFiles(x86)%\CMake\bin\cmake.exe" set "CMAKE=%ProgramFiles(x86)%\CMake\bin\cmake.exe"
if not defined CMAKE (
    for /f "delims=" %%i in ('where cmake 2^>nul') do (
        if not defined CMAKE (
            echo %%i | findstr /i "msys devkitpro" >nul || set "CMAKE=%%i"
        )
    )
)
if not defined CMAKE (
    echo [FALTA] CMake !MIN_CMAKE_VERSION! o posterior para !VS_GENERATOR!.
    echo         Instala CMake desde https://cmake.org/download/ y activa "Add CMake to PATH",
    echo         o agrega el componente "C++ CMake tools for Windows" en Visual Studio Installer.
    set "MISSING=1"
) else (
    set "CMAKE_VERSION="
    for /f "tokens=3" %%v in ('"!CMAKE!" --version') do if not defined CMAKE_VERSION set "CMAKE_VERSION=%%v"
    for /f "tokens=1,2 delims=." %%a in ("!CMAKE_VERSION!") do (
        set "CMAKE_MAJOR=%%a"
        set "CMAKE_MINOR=%%b"
    )
    if not defined CMAKE_MAJOR set "CMAKE_MAJOR=0"
    if not defined CMAKE_MINOR set "CMAKE_MINOR=0"
    set "CMAKE_TOO_OLD=0"
    if !CMAKE_MAJOR! LSS !MIN_CMAKE_MAJOR! (
        set "CMAKE_TOO_OLD=1"
    )
    if !CMAKE_MAJOR! EQU !MIN_CMAKE_MAJOR! if !CMAKE_MINOR! LSS !MIN_CMAKE_MINOR! set "CMAKE_TOO_OLD=1"
    if "!CMAKE_TOO_OLD!"=="1" (
        echo [FALTA] CMake !CMAKE_VERSION! es demasiado antiguo; !VS_GENERATOR! necesita CMake !MIN_CMAKE_VERSION! o posterior.
        set "MISSING=1"
    ) else (
        echo [OK] CMake !CMAKE_VERSION!: !CMAKE!
    )
)

rem SDL3 e ImGui son necesarios para compilar la aplicacion. No se descargan
rem automaticamente: el usuario puede revisar el comando antes de ejecutarlo.
where git >nul 2>nul
if not errorlevel 1 set "GIT_OK=1"
if not exist "externals\SDL\CMakeLists.txt" (
    echo [FALTA] Submodulo SDL3: externals\SDL
    set "MISSING=1"
)
if not exist "externals\imgui\imgui.cpp" (
    echo [FALTA] ImGui: externals\imgui
    set "MISSING=1"
)
set "SDK_FOUND=0"
for /d %%s in ("%ProgramFiles(x86)%\Windows Kits\10\Include\10.*") do if exist "%%s\um\Windows.h" set "SDK_FOUND=1"
if "!SDK_FOUND!"=="0" (
    echo [FALTA] Windows 10/11 SDK para compilar con MSVC.
    echo         Instala "Windows 10 SDK" o "Windows 11 SDK" desde Visual Studio Installer ^> Componentes individuales.
    set "MISSING=1"
)
rem VS2026 auto-selects the newest SDK, which may be a preview with broken UCRT paths.
rem Prefer the stable Windows 11 SDK 10.0.26100 when it is installed.
if exist "%ProgramFiles(x86)%\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64\ucrtd.lib" set "SDK_PLATFORM=x64,version=10.0.26100.0"
rem Some installers (GameInput, Application Verifier) write a 64-bit KitsRoot10 that points to
rem "Program Files\Windows Kits\10", which has no Lib folder. MSBuild then looks for the UCRT
rem (ucrtd.lib) there and CMake reports "No CMAKE_CXX_COMPILER could be found" (LNK1104).
rem Point MSBuild at the real SDK folder when the UCRT is there.
if exist "%ProgramFiles(x86)%\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64\ucrtd.lib" set "UCRTContentRoot=%ProgramFiles(x86)%\Windows Kits\10\"
if not exist "externals\SDL\CMakeLists.txt" if not exist "externals\imgui\imgui.cpp" (
    if "!GIT_OK!"=="1" (
        echo         Para inicializarlos: git submodule update --init --recursive
    ) else (
        echo         Instala Git for Windows y ejecuta: git submodule update --init --recursive
        echo         Descarga Git: https://git-scm.com/download/win
    )
) else if not exist "externals\SDL\CMakeLists.txt" (
    echo         Ejecuta: git submodule update --init externals/SDL
) else if not exist "externals\imgui\imgui.cpp" (
    echo         Ejecuta: git submodule update --init externals/imgui
)

rem Componentes opcionales: si faltan, CMake compila sin esa funcion.
if not exist "externals\dynarmic\CMakeLists.txt" (
    echo [OPCIONAL] Falta dynarmic: el emulador compilara sin JIT y sera mas lento.
    echo            Inicializa con: git submodule update --init --recursive
)
if not exist "externals\Vulkan-Headers\include\vulkan\vulkan.h" (
    echo [OPCIONAL] Falta Vulkan-Headers; se compilara sin el backend Vulkan.
    echo            Inicializa con: git submodule update --init externals/Vulkan-Headers
)
if not exist "externals\volk\volk.c" (
    echo [OPCIONAL] Falta volk; se compilara sin el backend Vulkan.
    echo            Inicializa con: git submodule update --init
)
if not exist "externals\mbedtls\CMakeLists.txt" (
    echo [OPCIONAL] Falta mbedTLS: los programas no tendran HTTPS/TLS.
    echo            Inicializa con: git submodule update --init --recursive
)

if "!MISSING!"=="1" (
    echo.
    echo No se puede compilar hasta instalar o preparar los requisitos marcados [FALTA].
    echo Revisa tambien la guia BUILDING.md.
    endlocal & exit /b 1
)

rem CMake no permite reutilizar un cache creado desde otra ruta del repositorio.
if exist "!BUILD_DIR!\CMakeCache.txt" (
    set "SOURCE_PATH=%CD:\=/%"
    findstr /i /l /c:"CMAKE_HOME_DIRECTORY:INTERNAL=!SOURCE_PATH!" "!BUILD_DIR!\CMakeCache.txt" >nul
    if errorlevel 1 (
        echo [FALTA] El directorio !BUILD_DIR! contiene una configuracion de otra ruta del repositorio.
        echo         Renombra o borra esa carpeta y vuelve a ejecutar build.bat.
        set "MISSING=1"
    )
    if "!VS_MAJOR!"=="18" (
        findstr /i /l /c:"CMAKE_GENERATOR_TOOLSET:INTERNAL=v143" "!BUILD_DIR!\CMakeCache.txt" >nul
        if not errorlevel 1 (
            echo [FALTA] La carpeta !BUILD_DIR! conserva la seleccion antigua del toolset v143, incompatible con esta configuracion de VS 2026.
            echo         Renombra o borra esa carpeta y vuelve a ejecutar build.bat.
            set "MISSING=1"
        )
    )
    if "!SDK_PLATFORM!"=="x64,version=10.0.26100.0" (
        findstr /i /e /c:"CMAKE_GENERATOR_PLATFORM:INTERNAL=x64" "!BUILD_DIR!\CMakeCache.txt" >nul
        if not errorlevel 1 (
            echo [FALTA] La carpeta !BUILD_DIR! esta configurada con otra plataforma de Windows SDK.
            echo         Renombra o borra esa carpeta y vuelve a ejecutar build.bat.
            set "MISSING=1"
        )
    )
)
if "!MISSING!"=="1" (
    echo.
    echo Corrige los requisitos marcados [FALTA] y vuelve a ejecutar build.bat.
    endlocal & exit /b 1
)

rem Qt es opcional. Usa Qt existente, lo descarga con Python/aqtinstall o se omite.
set "QT_VERSION=6.8.3"
set "QTARG="
if not defined NEXO2_NO_QT if not defined NEXO2_QT_DIR if exist "externals\qt\%QT_VERSION%\msvc2022_64\bin\windeployqt.exe" set "NEXO2_QT_DIR=%~dp0externals\qt\%QT_VERSION%\msvc2022_64"
if not defined NEXO2_NO_QT if not defined NEXO2_QT_DIR (
    for /d %%d in ("C:\Qt\6.*") do if exist "%%d\msvc2022_64\bin\windeployqt.exe" set "NEXO2_QT_DIR=%%d\msvc2022_64"
)
if not defined NEXO2_NO_QT if not defined NEXO2_QT_DIR (
    where py >nul 2>nul
    if errorlevel 1 where python >nul 2>nul
    if errorlevel 1 (
        echo [OPCIONAL] Python no esta instalado; no se podra descargar Qt automaticamente.
        echo            Para compilar solo NeXo2.exe, ejecuta: set NEXO2_NO_QT=1
        echo            Python: https://www.python.org/downloads/windows/
        set "NEXO2_NO_QT=1"
    ) else (
        call :get_qt
    )
)
if defined NEXO2_QT_DIR (
    echo [OK] Qt: !NEXO2_QT_DIR!
    set "QTARG="-DCMAKE_PREFIX_PATH=!NEXO2_QT_DIR!""
) else (
    echo [INFO] Se compilara sin la interfaz opcional NeXo2-Qt.exe.
)

echo.
echo === Configurando ===
"!CMAKE!" -B "!BUILD_DIR!" -G "!VS_GENERATOR!" !VS_TOOLSET_ARG! -A !SDK_PLATFORM! !QTARG!
if errorlevel 1 (
    echo.
    echo [ERROR] Fallo la configuracion. Comprueba el Windows SDK y los mensajes anteriores de CMake.
    if "!VS_MAJOR!"=="18" (
        echo         Diagnostico completo: !BUILD_DIR!\CMakeFiles\CMakeConfigureLog.yaml
        echo         Si aparece LNK1104 para ucrtd.lib, repara Windows 11 SDK 10.0.26100 y el componente Windows Universal C Runtime en Visual Studio Installer.
    )
    endlocal & exit /b 1
)
echo.
echo === Compilando Release ===
"!CMAKE!" --build "!BUILD_DIR!" --config Release
if errorlevel 1 (
    echo.
    echo [ERROR] Fallo la compilacion. Revisa el primer error de MSVC mostrado arriba.
    endlocal & exit /b 1
)
if /i "%~1"=="test" "!BUILD_DIR!\Release\nexo2_tests.exe"
endlocal & exit /b %errorlevel%

:get_qt
echo [INFO] Qt 6 no esta instalado; se intentara descargar Qt %QT_VERSION% (~600 MB).
set "PY="
where py >nul 2>nul && set "PY=py"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY exit /b 0
%PY% -m pip install --quiet --user aqtinstall
if errorlevel 1 (
    echo [AVISO] No se pudo instalar aqtinstall. Se continuara sin Qt.
    exit /b 0
)
%PY% -m aqt install-qt windows desktop %QT_VERSION% win64_msvc2022_64 -O "%~dp0externals\qt"
if errorlevel 1 (
    echo [AVISO] No se pudo descargar Qt. Se continuara sin NeXo2-Qt.exe.
    exit /b 0
)
if exist "externals\qt\%QT_VERSION%\msvc2022_64\bin\windeployqt.exe" set "NEXO2_QT_DIR=%~dp0externals\qt\%QT_VERSION%\msvc2022_64"
exit /b 0
