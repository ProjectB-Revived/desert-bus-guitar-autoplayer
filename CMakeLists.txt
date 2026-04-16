cmake_minimum_required(VERSION 3.10)
project(DesertBusAutoPlayer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static linking for MSVC
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include_directories(include)

file(GLOB SOURCES "src/*.cpp")

add_executable(DesertBusAutoPlayer ${SOURCES})

target_link_libraries(DesertBusAutoPlayer Comdlg32.lib User32.lib)

if(MSVC)
    target_compile_options(DesertBusAutoPlayer PRIVATE /MT)
endif()
