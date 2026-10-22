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
