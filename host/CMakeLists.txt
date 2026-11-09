cmake_minimum_required(VERSION 3.20)
project(bcr2_host CXX)
set(CMAKE_CXX_STANDARD 20)

# the recompiled game code, compiled straight into the exe so the import
# stubs below can override the weak thunk functions
file(GLOB PPC_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../ppc/*.cpp")

add_executable(bcr2 main.cpp extract.cpp firstrun.cpp imports.cpp hooks.cpp gpu.cpp shaderpack.cpp settings.cpp upgradecycle.cpp kbm.cpp audio.cpp generated/import_stubs.cpp ${PPC_SRC})
set_source_files_properties(gpu.cpp shaderpack.cpp PROPERTIES COMPILE_OPTIONS "/O2")   # the renderer decodes textures and copies vertices
target_link_libraries(bcr2 PRIVATE d3d11 dxgi d3dcompiler user32 gdi32 ole32 windowscodecs comdlg32 shell32)
target_link_options(bcr2 PRIVATE /MAP:bcr2.map)
target_include_directories(bcr2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ppc
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    C:/dev/XenonRecomp/thirdparty/simde)
set(BCR2_GUEST_OPT "/O2" CACHE STRING "Optimisation flag for the recompiled game code: /O2 (fastest), /Os or /O1 (smaller).")
option(BCR2_OPTIMIZE_GUEST "Optimise the recompiled game code." ON)
if(BCR2_OPTIMIZE_GUEST)
    # Guest memory is shared between threads and read through byte-swapping macros, so keep aliasing rules off.
    target_compile_options(bcr2 PRIVATE ${BCR2_GUEST_OPT} /w -msse4.1 /clang:-fno-strict-aliasing)
else()
    target_compile_options(bcr2 PRIVATE /Od /w -msse4.1)
endif()
# first-time setup tool: unpacks the game from its content package (also built into bcr2.exe as --extract)
add_executable(bcr2_extract extract.cpp)
target_compile_definitions(bcr2_extract PRIVATE BCR2_EXTRACT_MAIN)
target_compile_options(bcr2_extract PRIVATE /O2)
option(BCR2_STATIC_CRT "Link the C/C++ runtime statically, so a release needs no Visual C++ redistributable." OFF)
if(BCR2_STATIC_CRT)
    set_property(TARGET bcr2 bcr2_extract PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
