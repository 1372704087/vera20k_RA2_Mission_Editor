cmake_minimum_required(VERSION 3.20)
project(vera20k-map-tool LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui)

add_executable(map-tool WIN32
    src/main.cpp
    src/ra2map.cpp
    src/ra2map.h
    src/gamefiles.cpp
    src/gamefiles.h
    src/theater.cpp
    src/theater.h
    3rdparty/minilzo/minilzo.c
    3rdparty/minilzo/minilzo.h
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(map-tool PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    3rdparty/minilzo
)

target_link_libraries(map-tool PRIVATE d3d11 dxgi dwmapi ole32)
target_compile_options(map-tool PRIVATE /utf-8)
