cmake_minimum_required(VERSION 3.20)
project(NeXo2 LANGUAGES CXX C)

# 1. Configuración de C++
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. Rutas del Proyecto
set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(EXT_DIR "${CMAKE_SOURCE_DIR}/externals")
set(IMGUI_DIR "${EXT_DIR}/imgui")

# 3. Localizar SDL3 (Asumiendo que está en externals/SDL)
add_subdirectory("${EXT_DIR}/SDL" EXCLUDE_FROM_ALL)

# 4. Fuentes del Emulador (Core)
set(SOURCES
    "${SRC_DIR}/main.cpp"
    "${SRC_DIR}/core/arm64/interpreter.cpp"
    "${SRC_DIR}/core/arm64/interpreter.hpp"
    "${SRC_DIR}/core/arm64/cpu_state.hpp"
    "${SRC_DIR}/core/memory/memory.hpp"
)

# 5. Fuentes de ImGui (Necesario para la interfaz)
# Asegúrate de que estos archivos existan en externals/imgui/
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
)

# 6. Crear el Ejecutable
add_executable(${PROJECT_NAME} ${SOURCES} ${IMGUI_SOURCES})

# 7. Directorios de Inclusión (Donde el compilador busca los .h y .hpp)
target_include_directories(${PROJECT_NAME} PRIVATE 
    "${SRC_DIR}"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${EXT_DIR}/SDL/include"
)

# 8. Vincular Librerías (Linker)
# SDL3-shared es el target por defecto que genera el CMake de SDL
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3-shared)

# 9. Configuración de salida (Opcional, organiza los .exe)
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 10. Copiar SDL3.dll al lado del ejecutable automáticamente (Solo Windows)
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL3-shared>
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
endif()