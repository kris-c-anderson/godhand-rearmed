# God Hand game layer (godhand.exe) and the recompiled game code. Adapted from the Urban Reign port's src/CMakeLists.txt.

# GODHAND_ARCH: instruction set the compiler may use in the generated code (game code + VU1 AOT, the bulk of the build).
# SSE41 (default): one exe for every CPU with SSE4.1 (MSVC has no /arch:SSE4.2, so its own code stays SSE2 and the
# runtime's explicit SSE4.1 intrinsics still apply). AVX2: only to compare speed; the exe then needs an AVX2 CPU.
set(GODHAND_ARCH "SSE41" CACHE STRING "Instruction set of the generated code: SSE41 (default) or AVX2 (comparison only)")
set_property(CACHE GODHAND_ARCH PROPERTY STRINGS SSE41 AVX2)
if(GODHAND_ARCH STREQUAL "AVX2")
    set(GODHAND_ARCH_FLAG /arch:AVX2)
else()
    set(GODHAND_ARCH_FLAG "")
endif()

set(GODHAND_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated")
set(GODHAND_RUNTIME_INCLUDE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/include")

if(NOT EXISTS "${GODHAND_GENERATED_DIR}/register_functions.cpp")
    if(GODHAND_BUILD_RECOMP)
        # Configure for building ps2_recomp only (build.py generate on a fresh tree): no godhand target yet.
        message(WARNING
            "Recompiled game code not found in ${GODHAND_GENERATED_DIR}; configuring without the godhand target.\n"
            "Build ps2_recomp, then run: python build.py generate")
        return()
    endif()
    message(FATAL_ERROR
        "Recompiled game code not found in ${GODHAND_GENERATED_DIR}.\n"
        "Put your own God Hand (USA) disc image in game/ and run: python build.py generate")
endif()

file(GLOB GODHAND_GENERATED_SOURCES CONFIGURE_DEPENDS "${GODHAND_GENERATED_DIR}/*.cpp")

set(GODHAND_HOST_SOURCES
    main.cpp
    godhand/button_prompt_sheets.cpp
    godhand/button_prompts.cpp
    godhand/crate_prompts.cpp
    godhand/call_profile.cpp
    godhand/call_trace.cpp
    godhand/movie_watchdog.cpp
    godhand/frame_patches.cpp
    godhand/game_options.cpp
    godhand/character_mods.cpp
    godhand/widescreen.cpp
    godhand/hud_layout.cpp
    godhand/ini_file.cpp
    godhand/counter_finder.cpp
    godhand/pc_sampler.cpp
    godhand/function_hooks.cpp
    godhand/game_disc.cpp
    godhand/hang_dump.cpp
    godhand/hle_bindings.cpp
    godhand/host_window.cpp
    godhand/log.cpp
    godhand/prompt_art.cpp
    godhand/sha256.cpp
    godhand/thread_policy.cpp
)

# godhand.ini (top level) is built into godhand.exe as its default settings: godhand_generated/godhand/default_ini.h.
# The header is rewritten only when the text changes, so editing godhand.ini rebuilds main.cpp alone.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/godhand.ini")
file(READ "${CMAKE_SOURCE_DIR}/godhand.ini" GODHAND_DEFAULT_INI)
string(LENGTH "${GODHAND_DEFAULT_INI}" GODHAND_DEFAULT_INI_LENGTH)
if(GODHAND_DEFAULT_INI_LENGTH GREATER 16000)
    message(FATAL_ERROR "godhand.ini is ${GODHAND_DEFAULT_INI_LENGTH} bytes; MSVC string literals stop at 16380 (split it)")
endif()
if(GODHAND_DEFAULT_INI MATCHES "\\)GHINI\"")
    message(FATAL_ERROR "godhand.ini must not contain )GHINI\" (the raw string delimiter)")
endif()
set(GODHAND_INI_HEADER "${CMAKE_CURRENT_BINARY_DIR}/godhand_generated/godhand/default_ini.h")
file(WRITE "${GODHAND_INI_HEADER}.tmp"
    "// Generated from godhand.ini by src/CMakeLists.txt. Do not edit.\n"
    "#pragma once\n"
    "namespace godhand\n"
    "{\n"
    "    inline constexpr const char *kDefaultIni = R\"GHINI(${GODHAND_DEFAULT_INI})GHINI\";\n"
    "}\n")
file(COPY_FILE "${GODHAND_INI_HEADER}.tmp" "${GODHAND_INI_HEADER}" ONLY_IF_DIFFERENT)

# Does the runtime have ps2_host_display::setHeadless (LightVelox/PS2Recomp sotc-port 29d26f9)? The Urban Reign runtime
# (2fa53c4) does not; godhand/host_window.cpp then hides the raylib window itself.
file(STRINGS "${GODHAND_RUNTIME_INCLUDE}/runtime/ps2_display_options.h" GODHAND_SET_HEADLESS_DECL REGEX "void[ \t]+setHeadless[ \t]*\\(")
if(GODHAND_SET_HEADLESS_DECL)
    set(GODHAND_RUNTIME_HAS_SET_HEADLESS 1)
else()
    set(GODHAND_RUNTIME_HAS_SET_HEADLESS 0)
endif()
message(STATUS "God Hand: runtime setHeadless available: ${GODHAND_RUNTIME_HAS_SET_HEADLESS}")

# Reuse an already-built game code library when only host sources change (the full EE build is long).
set(GODHAND_GAME_CODE_LIBRARY "" CACHE FILEPATH "Reuse an unchanged generated EE library for host-only builds")
if(GODHAND_GAME_CODE_LIBRARY)
    if(NOT EXISTS "${GODHAND_GAME_CODE_LIBRARY}")
        message(FATAL_ERROR "Cached generated EE library does not exist: ${GODHAND_GAME_CODE_LIBRARY}")
    endif()
    add_library(godhand_game_code STATIC IMPORTED)
    set_target_properties(godhand_game_code PROPERTIES IMPORTED_LOCATION "${GODHAND_GAME_CODE_LIBRARY}")
    target_include_directories(godhand_game_code INTERFACE "${GODHAND_GENERATED_DIR}")
    target_link_libraries(godhand_game_code INTERFACE ps2_runtime)
    # The huge-function sources live in a sibling library built next to it.
    get_filename_component(GODHAND_GAME_CODE_LIBRARY_DIR "${GODHAND_GAME_CODE_LIBRARY}" DIRECTORY)
    target_link_libraries(godhand_game_code INTERFACE "${GODHAND_GAME_CODE_LIBRARY_DIR}/godhand_game_code_huge.lib")
else()
    # /d2OptimizeHugeFunctions forces full optimization of functions MSVC would otherwise skip. Fine for most files, but
    # one huge function (Urban Reign: ~11k guest instructions, 2 MB of C++) ran over 40 minutes with it. Files above the
    # threshold go into godhand_game_code_huge, which keeps MSVC's default huge-function handling.
    set(GODHAND_HUGE_SOURCE_BYTES 900000 CACHE STRING "Generated files larger than this skip /d2OptimizeHugeFunctions")
    set(GODHAND_HUGE_SOURCES "")
    foreach(source IN LISTS GODHAND_GENERATED_SOURCES)
        file(SIZE "${source}" sourceBytes)
        if(sourceBytes GREATER GODHAND_HUGE_SOURCE_BYTES AND NOT source MATCHES "register_functions\\.cpp$")
            list(APPEND GODHAND_HUGE_SOURCES "${source}")
        endif()
    endforeach()
    if(GODHAND_HUGE_SOURCES)
        list(REMOVE_ITEM GODHAND_GENERATED_SOURCES ${GODHAND_HUGE_SOURCES})
    endif()
    list(LENGTH GODHAND_HUGE_SOURCES GODHAND_HUGE_COUNT)
    message(STATUS "God Hand: ${GODHAND_HUGE_COUNT} generated files over ${GODHAND_HUGE_SOURCE_BYTES} bytes go to godhand_game_code_huge")

    # An empty static library is an error in some generators: a stub source keeps it valid when no file is huge.
    set(GODHAND_HUGE_STUB "${CMAKE_CURRENT_BINARY_DIR}/godhand_generated/huge_stub.cpp")
    file(WRITE "${GODHAND_HUGE_STUB}.tmp" "// Keeps godhand_game_code_huge non-empty.\nnamespace godhand { int hugeStub() { return 0; } }\n")
    file(COPY_FILE "${GODHAND_HUGE_STUB}.tmp" "${GODHAND_HUGE_STUB}" ONLY_IF_DIFFERENT)
    add_library(godhand_game_code_huge STATIC ${GODHAND_HUGE_SOURCES} "${GODHAND_HUGE_STUB}")
    target_include_directories(godhand_game_code_huge PUBLIC "${GODHAND_GENERATED_DIR}")
    target_link_libraries(godhand_game_code_huge PUBLIC ps2_runtime)
    # Each huge file needs several GB in MSVC's pass 2; compiling them all at once runs out of heap (C1002).
    set(GODHAND_HUGE_JOBS 2 CACHE STRING "Concurrent compiles of the huge generated sources (Ninja)")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS godhand_huge=${GODHAND_HUGE_JOBS})
    set_property(TARGET godhand_game_code_huge PROPERTY JOB_POOL_COMPILE godhand_huge)
    if(MSVC)
        target_compile_options(godhand_game_code_huge PRIVATE /bigobj /fp:precise ${GODHAND_ARCH_FLAG} /GS- /W0)
        # Huge files that still compile in reasonable time with /d2OptimizeHugeFunctions (file names, ';'-separated).
        # Urban Reign listed three of its own here; God Hand's are not measured yet (TODO after the first build).
        set(GODHAND_HUGE_OPTIMIZED "" CACHE STRING "Huge generated files still compiled with /d2OptimizeHugeFunctions")
        foreach(source IN LISTS GODHAND_HUGE_SOURCES)
            get_filename_component(sourceName "${source}" NAME)
            if(sourceName IN_LIST GODHAND_HUGE_OPTIMIZED)
                set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "/d2OptimizeHugeFunctions")
            endif()
        endforeach()
    endif()

    add_library(godhand_game_code STATIC ${GODHAND_GENERATED_SOURCES})
    target_link_libraries(godhand_game_code PUBLIC godhand_game_code_huge)
    target_include_directories(godhand_game_code PUBLIC "${GODHAND_GENERATED_DIR}")
    target_link_libraries(godhand_game_code PUBLIC ps2_runtime)
    set_target_properties(godhand_game_code PROPERTIES UNITY_BUILD OFF)
    target_precompile_headers(godhand_game_code PRIVATE
        "${GODHAND_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
        "${GODHAND_RUNTIME_INCLUDE}/ps2_runtime.h"
        "${GODHAND_GENERATED_DIR}/ps2_recompiled_functions.h"
        "${GODHAND_GENERATED_DIR}/ps2_recompiled_stubs.h"
        "${GODHAND_RUNTIME_INCLUDE}/ps2_syscalls.h"
        "${GODHAND_RUNTIME_INCLUDE}/ps2_stubs.h"
    )
    if(MSVC)
        # No /GL here, ever: whole-program optimization over ~10k generated files blew up link time and memory (Urban
        # Reign docs/ROADMAP.md A4), and God Hand's own Release/LTCG link died in "Generating code" (STATUS_2026-10-16.md).
        target_compile_options(godhand_game_code PRIVATE /bigobj /fp:precise ${GODHAND_ARCH_FLAG} /GS- /d2OptimizeHugeFunctions /W0 /MP $<$<CONFIG:RelWithDebInfo>:/Ob2>)
        # One ~100k-line table initializer that runs once at startup: compiles in seconds without PCH/optimizer.
        set_source_files_properties("${GODHAND_GENERATED_DIR}/register_functions.cpp" PROPERTIES
            COMPILE_OPTIONS "/Od" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()

# Recompiled SNR2 modules (God Hand's DLLs): 'python -I build.py modules <mod>' writes generated_modules/<mod>/ (the
# module's functions in namespace gh_<mod>, its header <mod>_functions.h and <mod>_pack.cpp; never
# register_functions.cpp, which would define the boot table's globals a second time). All modules go into ONE static
# library with ONE runtime-only precompiled header: every room pack is generated for the same slot base (see
# GODHAND_PORT_NOTES.md section 9), so their address-suffixed names repeat and live in per-module namespaces, and a
# per-module PCH (~160 MB each) would cost ~13 GB for 81 modules. The module headers are not in the PCH (each source
# includes its own "<mod>_functions.h" from its folder). godhand_generated/godhand/module_index.cpp lists the packs
# (and keeps the linker from dropping them).
set(GODHAND_MODULES_DIR "${CMAKE_SOURCE_DIR}/generated_modules")
file(GLOB GODHAND_MODULE_PACKS CONFIGURE_DEPENDS "${GODHAND_MODULES_DIR}/*/*_pack.cpp")
set(GODHAND_MODULE_SOURCES "")
set(GODHAND_MODULE_DECLS "")
set(GODHAND_MODULE_REFS "")
set(GODHAND_MODULE_COUNT 0)
foreach(modulePack IN LISTS GODHAND_MODULE_PACKS)
    get_filename_component(moduleDir "${modulePack}" DIRECTORY)
    get_filename_component(moduleName "${moduleDir}" NAME)
    if(NOT moduleName MATCHES "^[A-Za-z_][A-Za-z0-9_]*$" OR NOT modulePack MATCHES "/${moduleName}_pack\\.cpp$")
        message(FATAL_ERROR "generated_modules/${moduleName}: module folder must be a C identifier holding ${moduleName}_pack.cpp")
    endif()
    if(EXISTS "${moduleDir}/register_functions.cpp")
        message(FATAL_ERROR "${moduleDir}/register_functions.cpp would redefine the boot function table (rerun build.py modules)")
    endif()
    if(EXISTS "${moduleDir}/ps2_recompiled_functions.h" OR NOT EXISTS "${moduleDir}/${moduleName}_functions.h")
        message(FATAL_ERROR "${moduleDir} has the old module layout (rerun: python -I build.py modules --force ${moduleName})")
    endif()
    file(GLOB moduleSources CONFIGURE_DEPENDS "${moduleDir}/*.cpp")
    list(APPEND GODHAND_MODULE_SOURCES ${moduleSources})
    string(APPEND GODHAND_MODULE_DECLS "    extern const ModulePack kModulePack_${moduleName};\n")
    string(APPEND GODHAND_MODULE_REFS "&kModulePack_${moduleName}, ")
    math(EXPR GODHAND_MODULE_COUNT "${GODHAND_MODULE_COUNT} + 1")
endforeach()
set(GODHAND_MODULE_LIBRARIES "")
if(GODHAND_MODULE_SOURCES)
    add_library(godhand_modules STATIC ${GODHAND_MODULE_SOURCES})
    target_include_directories(godhand_modules PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(godhand_modules PUBLIC ps2_runtime)
    set_target_properties(godhand_modules PROPERTIES UNITY_BUILD OFF)
    target_precompile_headers(godhand_modules PRIVATE
        "${GODHAND_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
        "${GODHAND_RUNTIME_INCLUDE}/ps2_runtime.h"
        "${GODHAND_RUNTIME_INCLUDE}/ps2_syscalls.h"
        "${GODHAND_RUNTIME_INCLUDE}/ps2_stubs.h"
    )
    if(MSVC)
        # Same flags as godhand_game_code (no /GL).
        target_compile_options(godhand_modules PRIVATE /bigobj /fp:precise ${GODHAND_ARCH_FLAG} /GS- /d2OptimizeHugeFunctions /W0 /MP $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
    set(GODHAND_MODULE_LIBRARIES godhand_modules)
endif()
message(STATUS "God Hand: ${GODHAND_MODULE_COUNT} recompiled SNR2 module(s) in ${GODHAND_MODULES_DIR}")
set(GODHAND_MODULE_INDEX "${CMAKE_CURRENT_BINARY_DIR}/godhand_generated/godhand/module_index.cpp")
file(WRITE "${GODHAND_MODULE_INDEX}.tmp"
    "// Generated by src/CMakeLists.txt from generated_modules/*/*_pack.cpp. Do not edit.\n"
    "#include \"godhand/module_registry.h\"\n"
    "namespace godhand\n"
    "{\n"
    "${GODHAND_MODULE_DECLS}"
    "    const ModulePack *const *modulePacks(size_t &count)\n"
    "    {\n"
    "        static const ModulePack *const packs[] = {${GODHAND_MODULE_REFS}nullptr};\n"
    "        count = ${GODHAND_MODULE_COUNT}u;\n"
    "        return packs;\n"
    "    }\n"
    "}\n")
file(COPY_FILE "${GODHAND_MODULE_INDEX}.tmp" "${GODHAND_MODULE_INDEX}" ONLY_IF_DIFFERENT)

add_executable(godhand ${GODHAND_HOST_SOURCES} "${GODHAND_MODULE_INDEX}")
if(GODHAND_MODULE_LIBRARIES)
    target_link_libraries(godhand PRIVATE ${GODHAND_MODULE_LIBRARIES})
endif()
target_include_directories(godhand PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/godhand_generated"
    "${GODHAND_GENERATED_DIR}")
target_link_libraries(godhand PRIVATE godhand_game_code ps2_runtime)
target_compile_definitions(godhand PRIVATE GODHAND_RUNTIME_HAS_SET_HEADLESS=${GODHAND_RUNTIME_HAS_SET_HEADLESS})

if(MSVC)
    target_compile_options(godhand PRIVATE /fp:precise /W3 /utf-8)
    target_compile_definitions(godhand PRIVATE _CRT_SECURE_NO_WARNINGS GODHAND_ARCH_NAME="${GODHAND_ARCH}")
    # /FORCE:MULTIPLE: WinAPI vs raylib CloseWindow clash. Large stack: deep guest call chains run on the host stack.
    # /LTCG in RelWithDebInfo as Urban does: ps2_runtime is compiled with /GL by EnableFastReleaseMode
    # (runtime/ps2xRuntime/cmake/ReleaseMode.cmake, CMakeLists.txt:609-611), so the link does code generation for the
    # runtime objects only. The generated game code never gets /GL (above), so it is not part of that step.
    target_link_options(godhand PRIVATE /FORCE:MULTIPLE /STACK:16777216 $<$<CONFIG:RelWithDebInfo>:/LTCG>)
    target_link_libraries(godhand PRIVATE dbghelp winmm)
endif()

if(GODHAND_DEBUG_UI AND TARGET rlImGui)
    target_sources(godhand PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/ps2_debug_panel.cpp")
    target_include_directories(godhand PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib")
    target_link_libraries(godhand PRIVATE rlImGui imgui)
    target_compile_definitions(godhand PRIVATE PS2X_ENABLE_DEBUG_UI=1)
    # The launcher (src/godhand/launcher.cpp) draws with the same Dear ImGui.
    target_sources(godhand PRIVATE godhand/launcher.cpp godhand/launcher_platform.cpp)
    target_compile_definitions(godhand PRIVATE GODHAND_HAS_LAUNCHER=1)
endif()

# FFmpeg is off by default (GODHAND_FFMPEG); with it on, the runtime stages its DLLs next to the exe.
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(godhand)
endif()

# VU1 microcode compiled ahead of time: capture the code images with PS2X_VU1_CAPTURE, then run vu1_recomp (configure
# with -DGODHAND_BUILD_TOOLS=ON) into generated/vu1. Images not compiled stay interpreted. GENERIC (Urban, SotC).
file(GLOB GODHAND_VU1_SOURCES CONFIGURE_DEPENDS "${GODHAND_GENERATED_DIR}/vu1/*.cpp")
if(GODHAND_VU1_SOURCES)
    add_library(godhand_vu1_code STATIC ${GODHAND_VU1_SOURCES})
    target_include_directories(godhand_vu1_code PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/vu")
    target_link_libraries(godhand_vu1_code PUBLIC ps2_runtime)
    target_link_libraries(godhand PRIVATE godhand_vu1_code)
    target_compile_definitions(godhand PRIVATE GODHAND_HAS_VU1_PROGRAMS=1)
    if(MSVC)
        target_compile_options(godhand_vu1_code PRIVATE /bigobj /fp:precise ${GODHAND_ARCH_FLAG} /W0 /MP /d2OptimizeHugeFunctions $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
endif()

set_target_properties(godhand PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
