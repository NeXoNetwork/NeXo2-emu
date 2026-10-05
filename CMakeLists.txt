cmake_minimum_required(VERSION 3.25)
project(NeXo2 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(EXT_DIR "${CMAKE_SOURCE_DIR}/externals")
set(IMGUI_DIR "${EXT_DIR}/imgui")

option(NEXO2_BUILD_TESTS "Compilar los tests de la CPU (nexo2_tests)" ON)

# 1a. Nucleo del emulador (CPU, memoria, cargador NRO, kernel HLE) como libreria estatica.
#     Asi lo usan tanto el ejecutable como los tests, sin compilarlo dos veces.
add_library(nexo2_core STATIC
    "${SRC_DIR}/core/arm64/interpreter.cpp"
    "${SRC_DIR}/core/arm64/interpreter_dp_imm.cpp"
    "${SRC_DIR}/core/arm64/interpreter_dp_reg.cpp"
    "${SRC_DIR}/core/arm64/interpreter_branch.cpp"
    "${SRC_DIR}/core/arm64/interpreter_ldst.cpp"
    "${SRC_DIR}/core/loader/nro.cpp"
    "${SRC_DIR}/core/hle/kernel.cpp"
    "${SRC_DIR}/core/hle/ipc.cpp"
    "${SRC_DIR}/core/hle/service.cpp"
    "${SRC_DIR}/core/hle/services/sm.cpp"
    "${SRC_DIR}/core/hle/services/set.cpp"
    "${SRC_DIR}/core/system.cpp"
)
target_include_directories(nexo2_core PUBLIC
    "${SRC_DIR}"
    "${SRC_DIR}/core"
    "${SRC_DIR}/core/arm64"
    "${SRC_DIR}/core/memory"
)

# 1b. Fuentes del ejecutable (interfaz, JIT, icono)
set(SOURCES
    "${SRC_DIR}/main.cpp"
    "${SRC_DIR}/core/arm64/jit_ballistic.cpp"
    "${SRC_DIR}/core/memory/memory.hpp"
    "${SRC_DIR}/resource.rc" # <--- Esto pone el icono al .exe
)

# 2. Fuentes ImGui
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
)

# 3. SDL3
add_subdirectory("${EXT_DIR}/SDL" EXCLUDE_FROM_ALL)

# 3b. Reaplica parches de compatibilidad MSVC al submodulo ballistic (idempotente).
#     Se ejecuta en cada configuracion, asi que sobrevive a 'git submodule update'.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/apply_ballistic_patches.py"
)

# 3c. Ballistic (JIT ARM64 -> IR).
#     OJO: necesita Python 3 para generar las tablas del decoder al compilar.
#     Desactivamos sus tests para acelerar el build.
set(BALLISTIC_ENABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${EXT_DIR}/ballistic" EXCLUDE_FROM_ALL)

add_executable(${PROJECT_NAME} ${SOURCES} ${IMGUI_SOURCES})

# 4. RUTAS DE INCLUSIÓN
target_include_directories(${PROJECT_NAME} PRIVATE
    "${SRC_DIR}"
    "${SRC_DIR}/core"
    "${SRC_DIR}/core/arm64"
    "${SRC_DIR}/core/memory"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${EXT_DIR}/SDL/include"
)

# Ballistic expone sus cabeceras (include/) de forma PUBLIC, asi que al
# enlazarlo ya podemos hacer #include "bal_engine.h" sin añadir rutas a mano.
target_link_libraries(${PROJECT_NAME} PRIVATE nexo2_core SDL3-shared Ballistic)

# 5. Copia de DLL y Assets
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3-shared>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    )
endif()

# 6. Tests de la CPU (no necesitan SDL ni Ballistic)
#    Ejecutar:  ctest -C Release --output-on-failure
if(NEXO2_BUILD_TESTS)
    enable_testing()
    add_executable(nexo2_tests
        "${CMAKE_SOURCE_DIR}/tests/test_main.cpp"
        "${CMAKE_SOURCE_DIR}/tests/cpu_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/loader_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/ipc_tests.cpp"
    )
    target_include_directories(nexo2_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    # Carpeta con hello.nro y demas archivos de prueba
    target_compile_definitions(nexo2_tests PRIVATE
        NEXO2_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/generated")
    target_link_libraries(nexo2_tests PRIVATE nexo2_core)
    add_test(NAME cpu_tests COMMAND nexo2_tests)
endif()
