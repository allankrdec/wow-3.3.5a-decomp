file(GLOB PRIVATE_SOURCES CONFIGURE_DEPENDS "*.c" "*.cpp")

set_source_files_properties(${PRIVATE_SOURCES} PROPERTIES LANGUAGE CXX)

add_library(jpeg STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(jpeg
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# libjpeg's INT32 is 'long', 32 bits on Windows but 64 on macOS, which doubles some tables past MAX_ALLOC_CHUNK
# (16 KB in jconfig.h) and makes their allocation fail. XMD_H tells jmorecfg.h that INT16/INT32 are already defined.
if (NOT WIN32)
    target_compile_definitions(jpeg PUBLIC XMD_H INT16=short INT32=int)
endif ()
