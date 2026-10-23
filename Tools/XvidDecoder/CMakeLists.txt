if (NOT WIN32)
    # macOS: the same decoder as a static library over ThirdParty's xvidcore target; DivxDecoder links it.
    add_library(XvidDecoder STATIC
        XvidDecoder.cpp
    )

    target_include_directories(XvidDecoder
        PRIVATE
            ${CMAKE_SOURCE_DIR}
    )

    target_link_libraries(XvidDecoder
        PRIVATE
            xvidcore
    )

    return()
endif ()

add_library(XvidDecoder SHARED
    XvidDecoder.cpp
    XvidDecoder.def
)

set_target_properties(XvidDecoder PROPERTIES
    OUTPUT_NAME XvidDecoder
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/Win
)

foreach (config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${config} config)
    set_target_properties(XvidDecoder PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_SOURCE_DIR}/bin/Win)
endforeach ()

target_include_directories(XvidDecoder
    PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/ThirdParty/xvidcore/include
)

target_link_libraries(XvidDecoder
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ThirdParty/xvidcore/lib/x64/libxvidcore.lib
)

target_compile_options(XvidDecoder
    PRIVATE
        $<$<CONFIG:Debug>:/MD>
)
