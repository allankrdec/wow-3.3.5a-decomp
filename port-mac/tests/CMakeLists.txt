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
