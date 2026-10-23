file(GLOB PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

if (WOWDECOMP_PLATFORM_MAC)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Mac/*.cpp" "Mac/*.mm")
elseif (WOWDECOMP_ARCH_64)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Win64/*.cpp")
else ()
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Win32/*.cpp")
endif ()

if (WOWDECOMP_PLATFORM_MAC)
    set(WOW_PLATFORM_DIR Mac)
    add_executable(Wow MACOSX_BUNDLE ${PRIVATE_SOURCES} ${PLATFORM_SOURCES})
    set(WOW_OUTPUT_NAME "World of Warcraft")
else ()
    set(WOW_PLATFORM_DIR Win)
    add_executable(Wow WIN32 ${PRIVATE_SOURCES} ${PLATFORM_SOURCES})
    if (WOWDECOMP_ARCH_64)
        set(WOW_OUTPUT_NAME Wow64)
    else ()
        set(WOW_OUTPUT_NAME Wow32)
    endif ()
endif ()

set_target_properties(Wow PROPERTIES
    OUTPUT_NAME ${WOW_OUTPUT_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/${WOW_PLATFORM_DIR}
)

foreach (config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${config} config)
    set_target_properties(Wow PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_SOURCE_DIR}/bin/${WOW_PLATFORM_DIR})
endforeach ()

target_include_directories(Wow
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

if (WOWDECOMP_PLATFORM_MAC)
    set_target_properties(Wow PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Mac/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "World of Warcraft"
        MACOSX_BUNDLE_GUI_IDENTIFIER org.wowdecomp.wow
        MACOSX_BUNDLE_BUNDLE_VERSION 3.3.5.12340
        MACOSX_BUNDLE_SHORT_VERSION_STRING 3.3.5
        MACOSX_BUNDLE_ICON_FILE Wow.icns
    )

    # Bundle icon from the same waystone.ico the Windows executables use.
    set(WOW_ICONSET ${CMAKE_CURRENT_BINARY_DIR}/Wow.iconset)
    set(WOW_ICNS ${CMAKE_CURRENT_BINARY_DIR}/Wow.icns)
    add_custom_command(
        OUTPUT ${WOW_ICNS}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${WOW_ICONSET}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${WOW_ICONSET}
        COMMAND sips -s format png ${CMAKE_SOURCE_DIR}/waystone.ico --out ${WOW_ICONSET}/icon_32x32.png > /dev/null
        COMMAND sips -z 16 16 ${WOW_ICONSET}/icon_32x32.png --out ${WOW_ICONSET}/icon_16x16.png > /dev/null
        COMMAND sips -z 32 32 ${WOW_ICONSET}/icon_32x32.png --out ${WOW_ICONSET}/icon_16x16@2x.png > /dev/null
        COMMAND sips -z 64 64 ${WOW_ICONSET}/icon_32x32.png --out ${WOW_ICONSET}/icon_32x32@2x.png > /dev/null
        COMMAND sips -z 128 128 ${WOW_ICONSET}/icon_32x32.png --out ${WOW_ICONSET}/icon_128x128.png > /dev/null
        COMMAND sips -z 256 256 ${WOW_ICONSET}/icon_32x32.png --out ${WOW_ICONSET}/icon_128x128@2x.png > /dev/null
        COMMAND sips -z 256 256 ${WOW_ICONSET}/icon_32x32.png --out ${WOW_ICONSET}/icon_256x256.png > /dev/null
        COMMAND sips -z 512 512 ${WOW_ICONSET}/icon_32x32.png --out ${WOW_ICONSET}/icon_256x256@2x.png > /dev/null
        COMMAND sips -z 512 512 ${WOW_ICONSET}/icon_32x32.png --out ${WOW_ICONSET}/icon_512x512.png > /dev/null
        COMMAND iconutil -c icns ${WOW_ICONSET} -o ${WOW_ICNS}
        DEPENDS ${CMAKE_SOURCE_DIR}/waystone.ico
        VERBATIM
    )
    target_sources(Wow PRIVATE ${WOW_ICNS})
    set_source_files_properties(${WOW_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    # The linker signs only the executable; sign the whole bundle (ad-hoc) so
    # Info.plist and Resources are sealed too.
    add_custom_command(TARGET Wow POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:Wow>
        VERBATIM
    )
    # 8 MB main thread stack, like the fiber WinMain runs CommonMain on.
    target_link_options(Wow PRIVATE "LINKER:-stack_size,0x800000")
    target_link_libraries(Wow
        PRIVATE
            "-framework Cocoa"
            "-framework OpenGL"
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework AudioUnit"
            "-framework IOKit"
            "-framework Carbon"
            "-framework Security"
    )
else ()
    target_link_options(Wow PRIVATE "/INCREMENTAL:NO" "/ENTRY:start")
endif ()

target_link_libraries(Wow
    PRIVATE
        BLPFile
        Base
        BlizzardCore
        ComSat
        ConsoleBase
        Event
        FFX
        Frame
        FrameScript
        lua
        FrameXML
        Gx
        Gxu
        Images
        Model2
        Os
        Scrn
        Services
        ShaderEffect
        Tempest
        SoundEngine
        SteelseriesMouse
        Storm
        WoWCommon
        Battlenet
        Console
        ConsoleDevice
        DB
        Game
        Glue
        LiquidSystem
        Magic
        Minigame
        Net
        Object
        ObjectAlloc
        ObjectMgrClient
        SoundInterface
        UIUtil
        Ui
        WorldClient
        WowServices
        WowSvcs
)
