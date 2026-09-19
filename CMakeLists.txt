cmake_minimum_required(VERSION 3.20)
project(ps2recomp_ac5 C CXX)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GEN_OPT "-O1")
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

option(PS2_DIAG "compile the entry histogram and the guest-store watch in" OFF)

find_package(Threads REQUIRED)

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3)

set(PS2_GENERATED_DIR "generated" CACHE STRING
    "recompiler output directory, relative to the source tree")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/${PS2_GENERATED_DIR}")

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/ps2_code_*.c")
list(APPEND GENERATED_SOURCES
     "${GEN_DIR}/ps2_func_table.c"
     "${GEN_DIR}/ps2_image.c"
     "${GEN_DIR}/ps2_symbols.c")

if(NOT GENERATED_SOURCES)
  message(FATAL_ERROR
    "No generated sources. Run:\n"
    "  python -m ps2recomp <elf> -o generated --ida-db config/ida_db.json")
endif()

set(RUNTIME_SOURCES
    runtime/src/ps2_core.c
    runtime/src/ps2_hostprof.c
    runtime/src/ps2_hw.c
    runtime/src/ps2_gfxq.c
    runtime/src/ps2_gs.c
    runtime/src/ps2_vu.c
    runtime/src/ps2_ipu.c
    runtime/src/ps2_kernel.c
    runtime/src/ps2_spu2.c
    runtime/src/ps2_nustream.c
    runtime/src/ps2_memcard.c
    runtime/src/ps2_audio.c
    runtime/src/ps2_sif.c
    runtime/src/ps2_vfs.c
    runtime/src/ps2_hle_test.c
    runtime/src/ps2_hle_sif.c
    runtime/src/ps2_hle_cdvd.c
    runtime/src/ps2_hle_pad.c
    runtime/src/ps2_hle_iop.c
    runtime/src/ps2_video.c
    runtime/src/ps2_state.c
    runtime/src/ps2_capture.c
    runtime/src/ps2_statecap.c
    runtime/src/ps2_settings.c
    runtime/src/ps2_mod.c
    runtime/src/ps2_hook.c
    runtime/src/ps2_modapi.c
    runtime/src/ps2_patch.c
    runtime/src/ps2_params.c
    runtime/src/ps2_lua.c
    runtime/src/ps2_ui.cpp
    runtime/src/rn/rn_census.c
    runtime/src/rn/rn_2d.c
    runtime/src/rn/rn_dump.c
    runtime/src/rn/rn_vp.c
    runtime/src/rn/rn_vp_model.c
    runtime/src/rn/rn_vp_terrain.c
    runtime/src/rn/rn_vp_shadow.c
    runtime/src/rn/rn_vp_sprite.c
    runtime/src/rn/rn_vp_load.c
    runtime/src/rn/rn_vp_lit.c
    runtime/src/rn/rn_vp_fx.c
    runtime/src/rn/rn_vp_flat.c
    runtime/src/rn/rn_vpfuzz.c
    runtime/src/rn/rn_intent.c
    runtime/src/rn/rn_fixes.c
    runtime/src/rn/rn_sun.c
    runtime/src/rn/rn_sky.c
    runtime/src/rn/rn_tri3d.c
    runtime/src/rn/rn_billboard.c
    runtime/src/rn/rn_worldtri.c
    runtime/src/rn/rn_screen.c
    runtime/src/rn/rn_tap.c
    runtime/src/ps2_main.c)

set(LUA_DIR "${CMAKE_SOURCE_DIR}/third_party/lua")
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS "${LUA_DIR}/*.c")
add_library(lua54 STATIC ${LUA_SOURCES})
target_include_directories(lua54 PUBLIC "${LUA_DIR}")
set_source_files_properties(${LUA_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(lua54 PRIVATE -O2)

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
list(APPEND RUNTIME_SOURCES ${IMGUI_SOURCES})

add_executable(ac5 ${RUNTIME_SOURCES} ${GENERATED_SOURCES})
target_include_directories(ac5 PRIVATE runtime/include "${GEN_DIR}"
                                       ${IMGUI_DIR} ${IMGUI_DIR}/backends
                                       ${SDL3_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS})
target_link_directories(ac5 PRIVATE ${SDL3_LIBRARY_DIRS})
target_link_libraries(ac5 PRIVATE Threads::Threads m
                                  ${SDL3_LIBRARIES} Vulkan::Vulkan lua54)
target_link_options(ac5 PRIVATE -static-libstdc++ -static-libgcc)

option(PS2_HOOK_ENTRIES "patchable entry sleds in generated code, for mod hooks" ON)
set(GEN_COMPILE_OPTIONS "${GEN_OPT}")
if(PS2_HOOK_ENTRIES)
  list(APPEND GEN_COMPILE_OPTIONS "-fpatchable-function-entry=8")
endif()
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES
                            COMPILE_OPTIONS "${GEN_COMPILE_OPTIONS}")

set(HOT_SOURCES runtime/src/ps2_vu.c runtime/src/ps2_gs.c runtime/src/ps2_hw.c)
set_source_files_properties(${HOT_SOURCES} PROPERTIES COMPILE_OPTIONS "-O3")

if(PS2_DIAG)
  target_compile_definitions(ac5 PRIVATE PS2_DIAG=1)
endif()

target_compile_options(ac5 PRIVATE
    -fno-strict-aliasing
    -fwrapv
    -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

set(REPLAY_SOURCES ${RUNTIME_SOURCES})
list(REMOVE_ITEM REPLAY_SOURCES runtime/src/ps2_main.c
                                runtime/src/ps2_lua.c
                                runtime/src/ps2_modapi.c
                                runtime/src/ps2_mod.c
                                runtime/src/ps2_hook.c
                                runtime/src/ps2_patch.c
                                runtime/src/ps2_params.c
                                runtime/src/rn/rn_intent.c
                                runtime/src/rn/rn_fixes.c
                                runtime/src/rn/rn_tap.c)
list(APPEND REPLAY_SOURCES runtime/src/ps2_replay.c)

add_executable(gsreplay ${REPLAY_SOURCES})
target_include_directories(gsreplay PRIVATE runtime/include "${GEN_DIR}"
                                            "${LUA_DIR}"
                                            ${IMGUI_DIR} ${IMGUI_DIR}/backends
                                            ${SDL3_INCLUDE_DIRS}
                                            ${Vulkan_INCLUDE_DIRS})
target_link_directories(gsreplay PRIVATE ${SDL3_LIBRARY_DIRS})
target_link_libraries(gsreplay PRIVATE Threads::Threads m
                                       ${SDL3_LIBRARIES} Vulkan::Vulkan)
target_link_options(gsreplay PRIVATE -static-libstdc++ -static-libgcc)
if(PS2_DIAG)
  target_compile_definitions(gsreplay PRIVATE PS2_DIAG=1)
endif()

target_compile_options(gsreplay PRIVATE
    -fno-strict-aliasing -fwrapv
    -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(NOT GLSLC)
  message(FATAL_ERROR "glslc not found; it ships with the Vulkan SDK")
endif()

set(SHADER_OUT "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT}")
set(SPV_FILES)
foreach(SH gs.vert gs.frag gs_zcopy.frag colclip.frag present.vert present.frag
           rn_vis.frag rn_sun.vert rn_sun.frag mesh.vert mesh.frag)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${SH}.spv"
    COMMAND "${GLSLC}" -O "${CMAKE_SOURCE_DIR}/runtime/shaders/${SH}"
            -o "${SHADER_OUT}/${SH}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SH}"
    COMMENT "glslc ${SH}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${SH}.spv")
endforeach()
add_custom_target(shaders ALL DEPENDS ${SPV_FILES})
add_dependencies(ac5 shaders)
add_dependencies(gsreplay shaders)

include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c23")
check_c_source_compiles("
void g(void){}
void f(void){ [[gnu::musttail]] return g(); }
int main(void){ f(); return 0; }" HAVE_MUSTTAIL)
if(NOT HAVE_MUSTTAIL)
  message(WARNING
    "This compiler has no musttail attribute. Cross-function guest loops will "
    "consume stack. GCC 15+ or Clang 13+ is required for a correct build.")
endif()
