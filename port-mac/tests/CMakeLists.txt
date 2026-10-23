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

# Stages E4/E7 test of the window and input layer: WindowTest [seconds] [--synth] [--fullscreen] [--alert]; --synth runs
# the automated checks (with WOWDECOMP_MAC_BACKGROUND=1 the window stays behind and the real cursor is left alone).
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

# Stage E7 test: decodes frames of a game movie through the DivxDecoder API (MovieTest <game folder> [movie] [frames] [out.ppm]).
add_executable(MovieTest MovieTest.cpp)

target_include_directories(MovieTest
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(MovieTest
    PRIVATE
        DivxDecoder
)

# Stage E7 test: the crash handler (signals → Errors/<date> Crash.txt, SErr handlers, death by the signal) in forked children.
add_executable(CrashTest CrashTest.cpp)

target_link_libraries(CrashTest
    PRIVATE
        Storm BlizzardCore Os Base
        "-framework Cocoa" "-framework Carbon" "-framework Security" "-framework IOKit"
)

# Stage E7 test: Blizzard::URL::DownloadURL and OsURLDownload (NSURLSession) against a local HTTP server started by the test, plus https://example.com/ (HttpTest [--no-internet]).
add_executable(HttpTest HttpTest.cpp)

target_link_libraries(HttpTest
    PRIVATE
        Os Storm BlizzardCore Base
        "-framework Cocoa" "-framework Carbon" "-framework Security" "-framework IOKit"
)
