file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

# Direct3D is Windows-only; macOS uses the OpenGL backend (its NSOpenGL part is in CGxDeviceOpenGL/Mac).
if (NOT WIN32)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDevice(D3d|D3d9Ex)/")
endif ()

add_library(Gx STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Gx
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

if (WIN32)
    target_link_libraries(Gx
        PRIVATE
            d3d9.lib
            nvapi
            opengl32.lib
    )
endif ()

if (APPLE)
    target_link_libraries(Gx
        PRIVATE
            "-framework Cocoa"
            "-framework OpenGL"
    )
endif ()
