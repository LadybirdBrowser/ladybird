include(woff2)

set(SOURCES
    AffineTransform.cpp
    AntiAliasingPainter.cpp
    Bitmap.cpp
    CMYKBitmap.cpp
    Color.cpp
    DeltaE.cpp
    EdgeFlagPathRasterizer.cpp
    Filters/ColorBlindnessFilter.cpp
    Filters/FastBoxBlurFilter.cpp
    Filters/LumaFilter.cpp
    Filters/StackBlurFilter.cpp
    FontCascadeList.cpp
    Font/Emoji.cpp
    Font/Font.cpp
    Font/FontDatabase.cpp
    Font/OpenType/Cmap.cpp
    Font/OpenType/Font.cpp
    Font/OpenType/Glyf.cpp
    Font/OpenType/Hinting/Opcodes.cpp
    Font/OpenType/Tables.cpp
    Font/ScaledFont.cpp
    Font/Typeface.cpp
    Font/VectorFont.cpp
    Font/WOFF/Font.cpp
    Font/WOFF2/Font.cpp
    GradientPainting.cpp
    ICC/BinaryWriter.cpp
    ICC/Enums.cpp
    ICC/Profile.cpp
    ICC/Tags.cpp
    ICC/TagTypes.cpp
    ICC/WellKnownProfiles.cpp
    ImageFormats/AnimationWriter.cpp
    ImageFormats/BMPLoader.cpp
    ImageFormats/BMPWriter.cpp
    ImageFormats/BooleanDecoder.cpp
    ImageFormats/CCITTDecoder.cpp
    ImageFormats/GIFLoader.cpp
    ImageFormats/GIFWriter.cpp
    ImageFormats/ICOLoader.cpp
    ImageFormats/ILBMLoader.cpp
    ImageFormats/ImageDecoder.cpp
    ImageFormats/ISOBMFF/Boxes.cpp
    ImageFormats/ISOBMFF/JPEG2000Boxes.cpp
    ImageFormats/ISOBMFF/Reader.cpp
    ImageFormats/JBIG2Loader.cpp
    ImageFormats/JPEG2000Loader.cpp
    ImageFormats/JPEGLoader.cpp
    ImageFormats/JPEGXLLoader.cpp
    ImageFormats/JPEGWriter.cpp
    ImageFormats/PBMLoader.cpp
    ImageFormats/PGMLoader.cpp
    ImageFormats/PNGLoader.cpp
    ImageFormats/PNGWriter.cpp
    ImageFormats/PortableFormatWriter.cpp
    ImageFormats/PAMLoader.cpp
    ImageFormats/PPMLoader.cpp
    ImageFormats/QMArithmeticDecoder.cpp
    ImageFormats/TGALoader.cpp
    ImageFormats/TIFFLoader.cpp
    ImageFormats/TinyVGLoader.cpp
    ImageFormats/WebPLoader.cpp
    ImageFormats/WebPLoaderLossless.cpp
    ImageFormats/WebPLoaderLossy.cpp
    ImageFormats/WebPSharedLossless.cpp
    ImageFormats/WebPWriter.cpp
    ImageFormats/WebPWriterLossless.cpp
    ImmutableBitmap.cpp
    MedianCut.cpp
    Painter.cpp
    Palette.cpp
    Path.cpp
    PathClipper.cpp
    Point.cpp
    Rect.cpp
    ShareableBitmap.cpp
    Size.cpp
    SystemTheme.cpp
    TextDirection.cpp
    TextLayout.cpp
    Triangle.cpp
    VectorGraphic.cpp
)

serenity_lib(LibGfx gfx)
target_link_libraries(LibGfx PRIVATE LibCompress LibCore LibCrypto LibFileSystem LibRIFF LibTextCodec LibIPC LibUnicode LibURL)

# Third-party
target_include_directories(LibGfx PRIVATE ${WOFF2_INCLUDE_DIRS})
target_link_libraries(LibGfx PRIVATE ${WOFF2_LIBRARIES})
target_link_directories(LibGfx PRIVATE ${WOFF2_LIBRARY_DIRS})

set(generated_sources TIFFMetadata.h TIFFTagHandler.cpp)
list(TRANSFORM generated_sources PREPEND "ImageFormats/")

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
        OUTPUT  ${generated_sources}
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/TIFFGenerator.py" -o "${CMAKE_CURRENT_BINARY_DIR}/ImageFormats"
        DEPENDS "TIFFGenerator.py"
        VERBATIM
)
target_sources(LibGfx PRIVATE ${generated_sources})
add_custom_target(generate_tiff_files_handler DEPENDS ${generated_sources})
add_dependencies(all_generated generate_tiff_files_handler)
add_dependencies(LibGfx generate_tiff_files_handler)

list(TRANSFORM generated_sources PREPEND "${CMAKE_CURRENT_BINARY_DIR}/")
install(FILES ${generated_sources} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibGfx/ImageFormats")

find_package(JPEG REQUIRED)
target_include_directories(LibGfx PRIVATE ${JPEG_INCLUDE_DIRS})
target_link_libraries(LibGfx PRIVATE ${JPEG_LIBRARIES})
target_link_directories(LibGfx PRIVATE ${JPEG_LIBRARY_DIRS})
