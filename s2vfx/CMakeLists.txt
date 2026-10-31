cmake_minimum_required(VERSION 3.20)
project(s2vfxc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # one exe, no runtime to install

# The LZMA SDK's C sources (public domain, third_party/lzma), single threaded.
add_library(lzma STATIC
    third_party/lzma/LzmaDec.c
    third_party/lzma/LzmaEnc.c
    third_party/lzma/LzFind.c
    third_party/lzma/CpuArch.c)
target_include_directories(lzma PUBLIC third_party/lzma)
target_compile_definitions(lzma PUBLIC Z7_ST)

# The file format and the game's archives, shared by the compiler and the tests.
add_library(s2vfx_format STATIC
    src/vcs.cpp
    src/vpk.cpp)
target_include_directories(s2vfx_format PUBLIC src)
target_link_libraries(s2vfx_format PUBLIC lzma bcrypt)

add_executable(s2vfxc
    src/main.cpp
    src/compiler.cpp
    src/hlsl.cpp
    src/library.cpp
    src/render_state.cpp
    src/vfx.cpp)
target_link_libraries(s2vfxc PRIVATE s2vfx_format d3dcompiler)

add_executable(vcs_roundtrip tests/cpp/roundtrip.cpp)
target_link_libraries(vcs_roundtrip PRIVATE s2vfx_format)

if(MSVC)
    foreach(t s2vfx_format s2vfxc vcs_roundtrip)
        target_compile_options(${t} PRIVATE /W4 /utf-8)
        target_compile_definitions(${t} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endforeach()
endif()

# The compiler finds include/ by walking up from the program; an installed copy carries it beside the program.
install(TARGETS s2vfxc RUNTIME DESTINATION .)
install(DIRECTORY include/s2vfx DESTINATION include)
install(DIRECTORY examples DESTINATION .)
