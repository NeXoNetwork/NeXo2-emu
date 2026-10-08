cmake_minimum_required(VERSION 3.25)
project(NeXo2 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(EXT_DIR "${CMAKE_SOURCE_DIR}/externals")
set(IMGUI_DIR "${EXT_DIR}/imgui")

option(NEXO2_BUILD_APP "Compilar la aplicacion (necesita SDL3 e ImGui)" ON)
option(NEXO2_BUILD_TESTS "Compilar los tests de la CPU (nexo2_tests)" ON)
option(NEXO2_ENABLE_JIT "JIT dynarmic (ARM64 -> x86-64). Necesita externals/dynarmic y externals/ext-boost" ON)
option(NEXO2_ENABLE_VULKAN "GPU por Vulkan (fase 3). Necesita externals/Vulkan-Headers y externals/volk" ON)

# 1a. Nucleo del emulador (CPU, memoria, cargador NRO, kernel HLE) como libreria estatica.
#     Asi lo usan tanto el ejecutable como los tests, sin compilarlo dos veces.
add_library(nexo2_core STATIC
    "${SRC_DIR}/core/arm64/interpreter.cpp"
    "${SRC_DIR}/core/arm64/interpreter_dp_imm.cpp"
    "${SRC_DIR}/core/arm64/interpreter_dp_reg.cpp"
    "${SRC_DIR}/core/arm64/interpreter_fast.cpp"
    "${SRC_DIR}/core/arm64/interpreter_branch.cpp"
    "${SRC_DIR}/core/arm64/interpreter_ldst.cpp"
    "${SRC_DIR}/core/arm64/interpreter_simd_ldst.cpp"
    "${SRC_DIR}/core/arm64/interpreter_fp.cpp"
    "${SRC_DIR}/core/arm64/interpreter_simd.cpp"
    "${SRC_DIR}/core/arm64/interpreter_crypto.cpp"
    "${SRC_DIR}/core/arm64/fp_ops.cpp"
    "${SRC_DIR}/core/arm64/jit_dynarmic.cpp"
    "${SRC_DIR}/core/loader/nro.cpp"
    "${SRC_DIR}/core/hle/kernel.cpp"
    "${SRC_DIR}/core/hle/kernel_threads.cpp"
    "${SRC_DIR}/core/hle/ipc.cpp"
    "${SRC_DIR}/core/hle/service.cpp"
    "${SRC_DIR}/core/hle/display.cpp"
    "${SRC_DIR}/core/hle/input.cpp"
    "${SRC_DIR}/core/hle/services/sm.cpp"
    "${SRC_DIR}/core/hle/services/set.cpp"
    "${SRC_DIR}/core/hle/services/apm.cpp"
    "${SRC_DIR}/core/hle/services/applet.cpp"
    "${SRC_DIR}/core/hle/services/hid.cpp"
    "${SRC_DIR}/core/hle/services/time.cpp"
    "${SRC_DIR}/core/hle/services/fs.cpp"
    "${SRC_DIR}/core/hle/services/nvdrv.cpp"
    "${SRC_DIR}/core/hle/services/vi.cpp"
    "${SRC_DIR}/core/system.cpp"
    "${SRC_DIR}/video_core/gpu.cpp"
    "${SRC_DIR}/video_core/engines.cpp"
    "${SRC_DIR}/video_core/surface.cpp"
    "${SRC_DIR}/video_core/shader_decode.cpp"
    "${SRC_DIR}/video_core/shader_exec.cpp"
    "${SRC_DIR}/video_core/rasterizer.cpp"
    "${SRC_DIR}/video_core/texture.cpp"
)
# 1c. JIT: dynarmic (externals/dynarmic) traduce ARM64 a x86-64.
#     Sus cabeceras solo las ve jit_dynarmic.cpp. Necesita unas cabeceras de Boost
#     (externals/ext-boost, sin compilar nada de Boost).
if(NEXO2_ENABLE_JIT AND NOT EXISTS "${EXT_DIR}/dynarmic/CMakeLists.txt")
    message(WARNING "NEXO2_ENABLE_JIT: falta externals/dynarmic (git submodule update --init --recursive). Se compila sin JIT.")
    set(NEXO2_ENABLE_JIT OFF)
endif()
if(NEXO2_ENABLE_JIT AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64|x64)$")
    message(WARNING "NEXO2_ENABLE_JIT: de momento solo para PC x86-64. Se compila sin JIT.")
    set(NEXO2_ENABLE_JIT OFF)
endif()
if(NEXO2_ENABLE_JIT)
    set(BOOST_ROOT "${EXT_DIR}/ext-boost")
    set(Boost_INCLUDE_DIR "${EXT_DIR}/ext-boost")
    set(Boost_NO_SYSTEM_PATHS ON)
    set(CMAKE_POLICY_DEFAULT_CMP0167 OLD)   # FindBoost (cabeceras sueltas, sin BoostConfig.cmake)
    set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
    set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
    add_subdirectory("${EXT_DIR}/dynarmic" EXCLUDE_FROM_ALL)
    target_link_libraries(nexo2_core PRIVATE dynarmic)
    target_compile_definitions(nexo2_core PRIVATE NEXO2_HAS_JIT)
    message(STATUS "NeXo 2: JIT dynarmic activado")
endif()
# 1d. Vulkan (GPU fase 3): cabeceras de Khronos + volk, que carga el driver al arrancar
#     (no hace falta el Vulkan SDK). Sin los submodulos se compila solo con el dibujo por software.
if(NEXO2_ENABLE_VULKAN AND NOT (EXISTS "${EXT_DIR}/Vulkan-Headers/include/vulkan/vulkan.h" AND EXISTS "${EXT_DIR}/volk/volk.c"))
    message(WARNING "NEXO2_ENABLE_VULKAN: faltan externals/Vulkan-Headers o externals/volk (git submodule update --init). Se compila sin Vulkan.")
    set(NEXO2_ENABLE_VULKAN OFF)
endif()
if(NEXO2_ENABLE_VULKAN)
    target_sources(nexo2_core PRIVATE
        "${SRC_DIR}/video_core/vulkan/vk_device.cpp"
        "${EXT_DIR}/volk/volk.c"
    )
    target_include_directories(nexo2_core PUBLIC "${EXT_DIR}/Vulkan-Headers/include" "${EXT_DIR}/volk")
    target_compile_definitions(nexo2_core PUBLIC NEXO2_HAS_VULKAN VK_NO_PROTOTYPES)
    target_link_libraries(nexo2_core PRIVATE ${CMAKE_DL_LIBS})
    message(STATUS "NeXo 2: Vulkan activado")
endif()

# El dibujo por software reparte los pixeles entre varios hilos (std::thread)
find_package(Threads REQUIRED)
target_link_libraries(nexo2_core PUBLIC Threads::Threads)

# fp_ops.cpp cambia el modo de redondeo de la FPU del PC: el compilador no debe
# calcular nada en tiempo de compilacion ni reordenar las operaciones alrededor.
if(MSVC)
    set_source_files_properties("${SRC_DIR}/core/arm64/fp_ops.cpp" PROPERTIES COMPILE_OPTIONS "/fp:strict")
else()
    set_source_files_properties("${SRC_DIR}/core/arm64/fp_ops.cpp" PROPERTIES COMPILE_OPTIONS "-frounding-math")
endif()
target_include_directories(nexo2_core PUBLIC
    "${SRC_DIR}"
    "${SRC_DIR}/core"
    "${SRC_DIR}/core/arm64"
    "${SRC_DIR}/core/memory"
)

# Velocidad: en Release, optimizacion entre archivos (LTO, /GL + /LTCG en MSVC).
# Deja que el compilador meta en linea funciones de un .cpp dentro de otro.
include(CheckIPOSupported)
check_ipo_supported(RESULT NEXO2_IPO_OK OUTPUT NEXO2_IPO_MSG LANGUAGES CXX)
if(NEXO2_IPO_OK)
    set_property(TARGET nexo2_core PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

if(NEXO2_BUILD_APP)
# 1b. Fuentes del ejecutable (interfaz, icono)
set(SOURCES
    "${SRC_DIR}/main.cpp"
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

add_executable(${PROJECT_NAME} ${SOURCES} ${IMGUI_SOURCES})
if(NEXO2_IPO_OK)
    set_property(TARGET ${PROJECT_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

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

target_link_libraries(${PROJECT_NAME} PRIVATE nexo2_core SDL3-shared)

# 5. Copia de DLL y Assets
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3-shared>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    )
endif()
endif() # NEXO2_BUILD_APP

# 6. Tests de la CPU (no necesitan SDL)
#    Ejecutar:  ctest -C Release --output-on-failure
if(NEXO2_BUILD_TESTS)
    enable_testing()
    add_executable(nexo2_tests
        "${CMAKE_SOURCE_DIR}/tests/test_main.cpp"
        "${CMAKE_SOURCE_DIR}/tests/cpu_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/loader_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/ipc_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/services_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/simd_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/display_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/input_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/memory_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/decode_cache_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/threads_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/cpu_fuzz_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/gpu_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/gpu_shader_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/gpu_draw_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/gpu_texture_tests.cpp"
        "${CMAKE_SOURCE_DIR}/tests/vulkan_tests.cpp"
    )
    target_include_directories(nexo2_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    # Carpeta con hello.nro y demas archivos de prueba
    target_compile_definitions(nexo2_tests PRIVATE
        NEXO2_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/generated")
    target_link_libraries(nexo2_tests PRIVATE nexo2_core)
    add_test(NAME cpu_tests COMMAND nexo2_tests)
endif()
