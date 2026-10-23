cmake_minimum_required(VERSION 3.10)

if (${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(FATAL_ERROR
        "In-source builds not allowed.
        Please make a new directory (called a build directory) and run CMake from there.
        You may need to remove CMakeCache.txt."
    )
endif ()

# macOS builds are arm64 only (port-mac/docs/07-apple-silicon.md). Must be set before project().
if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "macOS architectures")
endif ()

project(wowdecomp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    set(WOWDECOMP_PLATFORM_WIN 1)
elseif (APPLE)
    set(WOWDECOMP_PLATFORM_MAC 1)
    enable_language(OBJCXX)
else ()
    message(FATAL_ERROR "wowdecomp targets Windows and macOS.")
endif ()

# x64 is the target. Win32 stays buildable: it is the configuration whose struct layouts can be
# checked against Wow.exe 3.3.5a (12340).
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WOWDECOMP_ARCH_64 1)
    add_compile_definitions(WOWDECOMP_ARCH_64=1)
elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(WOWDECOMP_ARCH_32 1)
    add_compile_definitions(WOWDECOMP_ARCH_32=1)
    # x87 floating point like the VS2005 original; keeps Win32 code comparable with the binary (re/tools/verify.py).
    if (MSVC)
        add_compile_options(/arch:IA32 /fp:fast)
    endif ()
    # 32-bit time_t like the original build: time/localtime/mktime resolve to _time32/_localtime32/_mktime32.
    add_compile_definitions(_USE_32BIT_TIME_T)
else ()
    message(FATAL_ERROR "Unsupported pointer size.")
endif ()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if (WOWDECOMP_PLATFORM_WIN)
    add_compile_definitions(
        WIN32
        _WINDOWS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
endif ()

if (WOWDECOMP_PLATFORM_MAC)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

    add_compile_definitions(
        WOWDECOMP_PLATFORM_MAC=1
        NOMINMAX
    )

    # Compat.h maps MSVC compiler/CRT spellings; Mac/Include has the CRT headers the macOS SDK lacks.
    include_directories(BEFORE ${CMAKE_SOURCE_DIR}/Engine/Source/Base/Platform/Mac/Include)
    add_compile_options(
        "SHELL:-include ${CMAKE_SOURCE_DIR}/Engine/Source/Base/Platform/Compat.h"
        # No fused multiply-add: keeps float results close to the x86 builds.
        -ffp-contract=off
        # MSVC accepts narrowing conversions in braced initializers and the C code compiled as C++ (jpeg, lua)
        # still uses 'register'.
        $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wno-c++11-narrowing>
        $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wno-register>
    )
endif ()

# Off by default: the client checks MPQ file MD5s (signaturefile) and GlueXML/FrameXML/AddOn .toc.sig files like the
# original. ON accepts unsigned or modified game data and interface files.
option(WOWDECOMP_DISABLE_SIGNATURE_CHECKS "Skip MPQ file signature and .toc signature checks" OFF)
if (WOWDECOMP_DISABLE_SIGNATURE_CHECKS)
    add_compile_definitions(DISABLE_SIGNATURE_CHECKS=1)
endif ()

# Third-party code is linked, not decompiled, for now.
if (EXISTS ${CMAKE_SOURCE_DIR}/ThirdParty/CMakeLists.txt)
    add_subdirectory(ThirdParty)
endif ()

add_subdirectory(Engine/Source/BLPFile)
add_subdirectory(Engine/Source/Base)
add_subdirectory(Engine/Source/BlizzardCore)
add_subdirectory(Engine/Source/ComSat)
add_subdirectory(Engine/Source/ConsoleBase)
add_subdirectory(Engine/Source/Event)
add_subdirectory(Engine/Source/FFX)
add_subdirectory(Engine/Source/Frame)
add_subdirectory(Engine/Source/FrameScript)
add_subdirectory(Engine/Source/FrameXML)
add_subdirectory(Engine/Source/Gx)
add_subdirectory(Engine/Source/Gxu)
add_subdirectory(Engine/Source/Images)
add_subdirectory(Engine/Source/jpeg)
add_subdirectory(Engine/Source/lua-5.1)
add_subdirectory(Engine/Source/Model2)
add_subdirectory(Engine/Source/Os)
add_subdirectory(Engine/Source/pcre)
add_subdirectory(Engine/Source/Scrn)
add_subdirectory(Engine/Source/Services)
add_subdirectory(Engine/Source/ShaderEffect)
add_subdirectory(Engine/Source/SoundEngine)
add_subdirectory(Engine/Source/SteelseriesMouse)
add_subdirectory(Engine/Source/Storm)
add_subdirectory(Engine/Source/Tempest)
add_subdirectory(WoW/Common)
add_subdirectory(WoW/Source/Battlenet)
add_subdirectory(WoW/Source/Console)
add_subdirectory(WoW/Source/ConsoleDevice)
add_subdirectory(WoW/Source/DB)
add_subdirectory(WoW/Source/Game)
add_subdirectory(WoW/Source/Glue)
add_subdirectory(WoW/Source/Magic)
add_subdirectory(WoW/Source/LiquidSystem)
add_subdirectory(WoW/Source/Minigame)
add_subdirectory(WoW/Source/Net)
add_subdirectory(WoW/Source/Object)
add_subdirectory(WoW/Source/ObjectAlloc)
add_subdirectory(WoW/Source/ObjectMgrClient)
add_subdirectory(WoW/Source/SoundInterface)
add_subdirectory(WoW/Source/UIUtil)
add_subdirectory(WoW/Source/Ui)
add_subdirectory(WoW/Source/WorldClient)
add_subdirectory(WoW/Source/WowServices)
add_subdirectory(WoW/Source/WowSvcs)
add_subdirectory(WoW/Source)
if (WIN32)
    add_subdirectory(Tools/WowClient)
endif ()
if ((WOWDECOMP_PLATFORM_WIN AND WOWDECOMP_ARCH_64) OR WOWDECOMP_PLATFORM_MAC)
    add_subdirectory(Tools/XvidDecoder)
endif ()
if (WOWDECOMP_PLATFORM_MAC)
    add_subdirectory(port-mac/tests)
endif ()

if (WOWDECOMP_PLATFORM_WIN)
    foreach (target Wow WowClient MapViewer M2Viewer GxTest MpqTest BlpTest DbcTest AssetCheck)
        if (TARGET ${target})
            target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/waystone.rc)
        endif ()
    endforeach ()
endif ()

# Platform folders: the modules glob every .cpp, so each platform drops the other platforms' folders here.
# Windows keeps Win32/ and Win64/ (their files are guarded by WOWDECOMP_ARCH_*) and drops Mac/. macOS drops
# Win32/, Win64/ and Windows/, plus the Windows-only files of common folders listed in
# port-mac/tools/mac-excluded.txt, and adds the Objective-C++ (.mm) files of its Mac/ folders.
function(wowdecomp_collect_targets dir out)
    get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach (subdir ${subdirs})
        wowdecomp_collect_targets(${subdir} subtargets)
        list(APPEND targets ${subtargets})
    endforeach ()
    set(${out} ${targets} PARENT_SCOPE)
endfunction ()

if (WOWDECOMP_PLATFORM_MAC)
    file(STRINGS ${CMAKE_SOURCE_DIR}/port-mac/tools/mac-excluded.txt WOWDECOMP_MAC_EXCLUDED REGEX "^[^#]")
endif ()

wowdecomp_collect_targets(${CMAKE_SOURCE_DIR} WOWDECOMP_TARGETS)
foreach (target ${WOWDECOMP_TARGETS})
    get_target_property(type ${target} TYPE)
    if (type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY" OR target STREQUAL "fmodex")
        continue ()
    endif ()

    get_target_property(sources ${target} SOURCES)
    get_target_property(source_dir ${target} SOURCE_DIR)
    set(kept)
    foreach (source ${sources})
        if (IS_ABSOLUTE ${source})
            set(path ${source})
        else ()
            set(path ${source_dir}/${source})
        endif ()
        file(RELATIVE_PATH relative ${CMAKE_SOURCE_DIR} ${path})

        if (WOWDECOMP_PLATFORM_WIN AND relative MATCHES "/Mac/")
            continue ()
        endif ()
        if (WOWDECOMP_PLATFORM_MAC)
            if (relative MATCHES "/(Win32|Win64|Windows)/")
                continue ()
            endif ()
            set(excluded FALSE)
            foreach (pattern ${WOWDECOMP_MAC_EXCLUDED})
                if (relative MATCHES "${pattern}")
                    set(excluded TRUE)
                endif ()
            endforeach ()
            if (excluded)
                continue ()
            endif ()
        endif ()
        list(APPEND kept ${source})
    endforeach ()

    if (WOWDECOMP_PLATFORM_MAC AND NOT target STREQUAL "Wow")
        file(GLOB_RECURSE objcxx_sources CONFIGURE_DEPENDS "${source_dir}/*.mm")
        list(FILTER objcxx_sources INCLUDE REGEX "/Mac/")
        list(APPEND kept ${objcxx_sources})
    endif ()

    set_property(TARGET ${target} PROPERTY SOURCES ${kept})
endforeach ()
