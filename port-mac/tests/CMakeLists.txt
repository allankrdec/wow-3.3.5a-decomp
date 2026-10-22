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

# for Os/Mac's game window until it exists (its functions are weak, so the real ones win).

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
