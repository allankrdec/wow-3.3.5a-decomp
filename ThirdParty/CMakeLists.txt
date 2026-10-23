add_library(zlib STATIC
    zlib/adler32.c
    zlib/compress.c
    zlib/crc32.c
    zlib/deflate.c
    zlib/gzio.c
    zlib/infback.c
    zlib/inffast.c
    zlib/inflate.c
    zlib/inftrees.c
    zlib/trees.c
    zlib/uncompr.c
    zlib/zutil.c
)

target_include_directories(zlib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/zlib
)

add_library(blast STATIC
    zlib/contrib/blast/blast.c
)

target_include_directories(blast
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/zlib/contrib/blast
)

add_library(bzip2 STATIC
    bzip2/blocksort.c
    bzip2/bzlib.c
    bzip2/compress.c
    bzip2/crctable.c
    bzip2/decompress.c
    bzip2/huffman.c
    bzip2/randtable.c
)

target_include_directories(bzip2
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/bzip2
)

add_library(lzma STATIC
    lzma/LzmaDecode.c
)

target_include_directories(lzma
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/lzma
)

if (WIN32)
    add_library(nvapi INTERFACE)

    target_include_directories(nvapi
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/nvapi
    )

    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_link_libraries(nvapi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/nvapi/amd64/nvapi64.lib)
    else ()
        target_link_libraries(nvapi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/nvapi/x86/nvapi.lib)
    endif ()
endif ()

add_library(freetype STATIC
    freetype/src/base/ftsystem.c
    freetype/src/base/ftinit.c
    freetype/src/base/ftdebug.c
    freetype/src/base/ftbase.c
    freetype/src/autohint/autohint.c
    freetype/src/cff/cff.c
    freetype/src/cid/type1cid.c
    freetype/src/psaux/psaux.c
    freetype/src/psnames/psnames.c
    freetype/src/raster/raster.c
    freetype/src/sfnt/sfnt.c
    freetype/src/smooth/smooth.c
    freetype/src/truetype/truetype.c
    freetype/src/type1/type1.c
    freetype/src/winfonts/winfnt.c
)

target_include_directories(freetype
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/freetype/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/freetype/src
)

add_library(expat STATIC
    expat/lib/xmlparse.c
    expat/lib/xmlrole.c
    expat/lib/xmltok.c
)

target_compile_definitions(expat
    PUBLIC
        _STATIC
    PRIVATE
        _LIB
)

if (WIN32)
    target_compile_definitions(expat PRIVATE COMPILED_FROM_DSP)
else ()
    target_include_directories(expat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/expat/mac)
endif ()

target_include_directories(expat
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/expat/lib
)

add_library(pcre STATIC
    pcre/pcre.c
    pcre/get.c
    pcre/study.c
    pcre/maketables.c
)

target_compile_definitions(pcre
    PUBLIC
        PCRE_STATIC
    PRIVATE
        SUPPORT_UTF8
        SUPPORT_UCP
)

target_include_directories(pcre
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/pcre
)

add_library(DivxDecoder INTERFACE)

target_include_directories(DivxDecoder
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/DivxDecoder
)

if (WOWDECOMP_ARCH_32)
    set(DIVXDECODER_IMPORT_LIB ${CMAKE_CURRENT_BINARY_DIR}/DivxDecoder.lib)

    execute_process(
        COMMAND ${CMAKE_AR} /NOLOGO /MACHINE:X86 /DEF:${CMAKE_CURRENT_SOURCE_DIR}/DivxDecoder/DivxDecoder.def /OUT:${DIVXDECODER_IMPORT_LIB}
        RESULT_VARIABLE DIVXDECODER_RESULT
    )

    if (NOT DIVXDECODER_RESULT EQUAL 0)
        message(FATAL_ERROR "DivxDecoder: creating the import library failed")
    endif ()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/DivxDecoder/DivxDecoder.def)

    target_link_libraries(DivxDecoder
        INTERFACE
            ${DIVXDECODER_IMPORT_LIB}
    )

    set(DIVXDECODER_DLL ${CMAKE_CURRENT_SOURCE_DIR}/DivxDecoder/DivxDecoder.dll)

    if (EXISTS ${DIVXDECODER_DLL})
        add_custom_target(DivxDecoderDll
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bin/Win
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DIVXDECODER_DLL} ${CMAKE_SOURCE_DIR}/bin/Win/DivxDecoder.dll
        )

        add_dependencies(DivxDecoder DivxDecoderDll)
    else ()
        message(WARNING "DivxDecoder: ${DIVXDECODER_DLL} is missing. Copy DivxDecoder.dll from a 3.3.5a (12340) client there; Win32 builds need it next to the executables.")
    endif ()
elseif (WIN32)
    target_link_libraries(DivxDecoder
        INTERFACE
            XvidDecoder
    )
else ()
    # The DivxDecoder API comes from Tools/XvidDecoder, built as a static library over the xvidcore below.
    target_link_libraries(DivxDecoder
        INTERFACE
            XvidDecoder
    )
endif ()

if (APPLE)
    # xvidcore 1.3.7 (GPL, see xvidcore/licenses/xvid.txt), plain C sources only: SRC_GENERIC from its
    # build/generic/sources.inc, no assembly. Windows keeps the prebuilt xvidcore/lib/x64/libxvidcore.lib.
    add_library(xvidcore STATIC
            xvidcore/src/bitstream/bitstream.c
            xvidcore/src/bitstream/cbp.c
            xvidcore/src/bitstream/mbcoding.c
            xvidcore/src/dct/fdct.c
            xvidcore/src/dct/idct.c
            xvidcore/src/dct/simple_idct.c
            xvidcore/src/decoder.c
            xvidcore/src/encoder.c
            xvidcore/src/image/colorspace.c
            xvidcore/src/image/font.c
            xvidcore/src/image/image.c
            xvidcore/src/image/interpolate8x8.c
            xvidcore/src/image/postprocessing.c
            xvidcore/src/image/qpel.c
            xvidcore/src/image/reduced.c
            xvidcore/src/motion/estimation_bvop.c
            xvidcore/src/motion/estimation_common.c
            xvidcore/src/motion/estimation_gmc.c
            xvidcore/src/motion/estimation_pvop.c
            xvidcore/src/motion/estimation_rd_based.c
            xvidcore/src/motion/estimation_rd_based_bvop.c
            xvidcore/src/motion/gmc.c
            xvidcore/src/motion/motion_comp.c
            xvidcore/src/motion/sad.c
            xvidcore/src/motion/vop_type_decision.c
            xvidcore/src/plugins/plugin_2pass1.c
            xvidcore/src/plugins/plugin_2pass2.c
            xvidcore/src/plugins/plugin_dump.c
            xvidcore/src/plugins/plugin_lumimasking.c
            xvidcore/src/plugins/plugin_psnr.c
            xvidcore/src/plugins/plugin_psnrhvsm.c
            xvidcore/src/plugins/plugin_single.c
            xvidcore/src/plugins/plugin_ssim.c
            xvidcore/src/prediction/mbprediction.c
            xvidcore/src/quant/quant_h263.c
            xvidcore/src/quant/quant_matrix.c
            xvidcore/src/quant/quant_mpeg.c
            xvidcore/src/utils/emms.c
            xvidcore/src/utils/mbtransquant.c
            xvidcore/src/utils/mem_align.c
            xvidcore/src/utils/mem_transfer.c
            xvidcore/src/utils/timer.c
            xvidcore/src/xvid.c
    )

    target_include_directories(xvidcore
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/xvidcore/include
    )

    target_compile_definitions(xvidcore
        PRIVATE
            ARCH_IS_GENERIC
            ARCH_IS_64BIT
            ARCH_IS_LITTLE_ENDIAN
    )

    target_compile_options(xvidcore
        PRIVATE
            -w
    )
endif ()

if (WIN32)
    add_library(lgLcd STATIC
        lgLcd/lglcd.cpp
    )

    target_include_directories(lgLcd
        PUBLIC
            ${CMAKE_SOURCE_DIR}
    )
endif ()
