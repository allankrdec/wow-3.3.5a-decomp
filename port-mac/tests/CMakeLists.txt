# Stage E3 test: reads the real game data through the Mac platform layer. Not run by CI (needs the game data).
add_executable(DataTest DataTest.cpp)

target_link_libraries(DataTest
    PRIVATE
        BLPFile Base BlizzardCore ComSat ConsoleBase Event FFX Frame FrameScript lua FrameXML Gx Gxu Images Model2 Os
        Scrn Services ShaderEffect Tempest SoundEngine SteelseriesMouse Storm WoWCommon Battlenet Console
        ConsoleDevice DB Game Glue LiquidSystem Magic Minigame Net Object ObjectAlloc ObjectMgrClient SoundInterface
        UIUtil Ui WorldClient WowServices WowSvcs
        "-framework Cocoa" "-framework Security" "-framework IOKit"
)

# Stage E4 test: the Mac OpenGL Gx device in the Os/Mac game window (needs a display; not run by CI).
add_executable(GxTest GxTest.mm)

target_link_libraries(GxTest
    PRIVATE
        BLPFile Base BlizzardCore ComSat ConsoleBase Event FFX Frame FrameScript lua FrameXML Gx Gxu Images Model2 Os
        Scrn Services ShaderEffect Tempest SoundEngine SteelseriesMouse Storm WoWCommon Battlenet Console
        ConsoleDevice DB Game Glue LiquidSystem Magic Minigame Net Object ObjectAlloc ObjectMgrClient SoundInterface
        UIUtil Ui WorldClient WowServices WowSvcs
        "-framework Cocoa" "-framework OpenGL" "-framework Carbon" "-framework Security" "-framework IOKit"
)

# Stage E4 manual test: opens the game window and prints the OS input events (WindowTest [seconds] [--synth] [--fullscreen]).
add_executable(WindowTest WindowTest.mm)

target_link_libraries(WindowTest
    PRIVATE
        BLPFile Base BlizzardCore ComSat ConsoleBase Event FFX Frame FrameScript lua FrameXML Gx Gxu Images Model2 Os
        Scrn Services ShaderEffect Tempest SoundEngine SteelseriesMouse Storm WoWCommon Battlenet Console
        ConsoleDevice DB Game Glue LiquidSystem Magic Minigame Net Object ObjectAlloc ObjectMgrClient SoundInterface
        UIUtil Ui WorldClient WowServices WowSvcs
        "-framework Cocoa" "-framework Carbon" "-framework Security" "-framework IOKit"
)

# Stage E5 test: WowConnection over BSD sockets against a local TCP server started by the test (no WoW server needed).
add_executable(NetTest NetTest.cpp)

target_link_libraries(NetTest
    PRIVATE
        WowServices ObjectAlloc ConsoleBase Os Storm BlizzardCore Base
        "-framework Cocoa" "-framework Carbon" "-framework Security" "-framework IOKit"
)

# Stage E6 test: FMOD Ex through the CoreAudio output plugin; plays ~12 s of quiet audio (SoundTest [game folder]).
add_executable(SoundTest SoundTest.cpp)

target_link_libraries(SoundTest
    PRIVATE
        fmodex BlizzardCore Storm Os Base
        "-framework Cocoa" "-framework Carbon" "-framework Security" "-framework IOKit" "-framework AudioUnit"
)
