cmake_minimum_required(VERSION 3.25)
project(NeXo2 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(EXT_DIR "${CMAKE_SOURCE_DIR}/externals")
set(IMGUI_DIR "${EXT_DIR}/imgui")

# 1. Recolectar fuentes de ImGui (Verifica que estas rutas existan)
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
)

# 2. Fuentes de NeXo 2
# Solo ponemos los .cpp aquí. Los .hpp los encuentra por el include_directories
set(SOURCES
    "${SRC_DIR}/main.cpp"
    "${SRC_DIR}/core/arm64/interpreter.cpp"
)

# 3. SDL3
add_subdirectory("${EXT_DIR}/SDL" EXCLUDE_FROM_ALL)

# 4. Crear el ejecutable (Aseguramos que SOURCES no esté vacío)
add_executable(${PROJECT_NAME} ${SOURCES} ${IMGUI_SOURCES})

# 5. Directorios de Inclusión (Aquí es donde el compilador buscará memory.hpp)
target_include_directories(${PROJECT_NAME} PRIVATE 
    "${SRC_DIR}"
    "${SRC_DIR}/core"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${EXT_DIR}/SDL/include"
)

target_link_libraries(${PROJECT_NAME} PRIVATE SDL3-shared)

# 6. Copia de DLL y Assets
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3-shared>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    )
endif()