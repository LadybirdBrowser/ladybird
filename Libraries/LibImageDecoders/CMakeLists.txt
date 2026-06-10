set(LIBGFX_IMAGE_FORMATS_DIR "${LADYBIRD_SOURCE_DIR}/Libraries/LibGfx/ImageFormats")

set(SOURCES
    ${LIBGFX_IMAGE_FORMATS_DIR}/AVIFLoader.cpp
    ${LIBGFX_IMAGE_FORMATS_DIR}/BMPLoader.cpp
    ${LIBGFX_IMAGE_FORMATS_DIR}/CCITTDecoder.cpp
    ${LIBGFX_IMAGE_FORMATS_DIR}/GIFLoader.cpp
    ${LIBGFX_IMAGE_FORMATS_DIR}/ICOLoader.cpp
    ${LIBGFX_IMAGE_FORMATS_DIR}/ImageDecoder.cpp
    ${LIBGFX_IMAGE_FORMATS_DIR}/JPEGLoader.cpp
    ${LIBGFX_IMAGE_FORMATS_DIR}/JPEGXLLoader.cpp
    ${LIBGFX_IMAGE_FORMATS_DIR}/PNGLoader.cpp
    ${LIBGFX_IMAGE_FORMATS_DIR}/TIFFLoader.cpp
    ${LIBGFX_IMAGE_FORMATS_DIR}/WebPLoader.cpp
)

ladybird_lib(LibImageDecoders imagedecoders)

target_link_libraries(LibImageDecoders PRIVATE
    LibCompress
    LibCore
    LibCrypto
    LibFileSystem
    LibGfx
    LibIPC
    LibSync
    LibTextCodec
    LibUnicode
    JPEG::JPEG
    PNG::PNG
    WebP::webp
    WebP::webpdecoder
    WebP::webpdemux
    WebP::libwebpmux
    avif
)

target_include_directories(LibImageDecoders PRIVATE ${WUFFS_INCLUDE_DIR})

set(generated_sources TIFFMetadata.h TIFFTagHandler.cpp)
list(TRANSFORM generated_sources PREPEND "${CMAKE_BINARY_DIR}/Libraries/LibGfx/ImageFormats/")

add_custom_command(
        OUTPUT  ${generated_sources}
        COMMAND ${Python3_EXECUTABLE} "${LADYBIRD_SOURCE_DIR}/Libraries/LibGfx/TIFFGenerator.py" -o "${CMAKE_BINARY_DIR}/Libraries/LibGfx/ImageFormats"
        DEPENDS "${LADYBIRD_SOURCE_DIR}/Libraries/LibGfx/TIFFGenerator.py"
        VERBATIM
)
target_sources(LibImageDecoders PRIVATE ${generated_sources})
add_custom_target(generate_tiff_files_handler DEPENDS ${generated_sources})
add_dependencies(ladybird_codegen_accumulator generate_tiff_files_handler)
add_dependencies(LibImageDecoders generate_tiff_files_handler)

if (ENABLE_INSTALL_HEADERS)
    install(FILES ${generated_sources} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibGfx/ImageFormats")
endif()

set(CMAKE_REQUIRED_LIBRARIES PNG::PNG)
check_c_source_compiles([=[
    #include <png.h>
    #if !defined(PNG_APNG_SUPPORTED) || !defined(PNG_READ_APNG_SUPPORTED) || !defined(PNG_READ_APNG_SUPPORTED)
    #error "APNG support is required"
    #endif
    int main() {}
]=] LIBPNG_HAS_APNG)
unset(CMAKE_REQUIRED_LIBRARIES)

if (NOT LIBPNG_HAS_APNG)
    message(FATAL_ERROR "libpng does not support APNG, which is required by LibImageDecoders.")
endif()

if (NOT ANDROID)
    target_link_libraries(LibImageDecoders PRIVATE PkgConfig::Jxl)
else()
    target_link_libraries(LibImageDecoders PRIVATE libjxl::libjxl hwy::hwy)
endif()
