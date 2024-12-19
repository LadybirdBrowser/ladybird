set(SOURCES
    Brotli.cpp
    BrotliDictionary.cpp
    Deflate.cpp
    Lzma.cpp
    Lzma2.cpp
    PackBitsDecoder.cpp
    Xz.cpp
    Zlib.cpp
    Gzip.cpp
)

serenity_lib(LibCompress compress)
target_link_libraries(LibCompress PRIVATE LibCore LibCrypto)

# FIXME: Get rid of this when #embed becomes available.
# Assuming xxd is available because it is part of the official git distro for Windows.
if (WIN32)
    set(BINARY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/BrotliDictionary.cpp.dict.bin)
    set(HEADER_FILE ${CMAKE_CURRENT_BINARY_DIR}/BrotliDictionary.dict.h)

    add_custom_command(
        OUTPUT ${HEADER_FILE}
        COMMAND xxd -i -n brotli_dictionary_data ${BINARY_FILE} ${HEADER_FILE}
        DEPENDS ${BINARY_FILE}
        COMMENT "Generating ${HEADER_FILE} from ${BINARY_FILE}"
    )

    add_custom_target(GenerateHeader ALL DEPENDS ${HEADER_FILE})
    add_dependencies(LibCompress GenerateHeader)
endif()
