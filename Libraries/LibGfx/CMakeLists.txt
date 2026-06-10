set(SOURCES
    AffineTransform.cpp
    Bitmap.cpp
    BitmapExport.cpp
    BitmapSequence.cpp
    CMYKBitmap.cpp
    Color.cpp
    ColorConversion.cpp
    ColorSpace.cpp
    CornerRadii.cpp
    Cursor.cpp
    Filter.cpp
    Font/Font.cpp
    Font/FontDatabase.cpp
    Font/FontSupport.cpp
    Font/FontVariationSettings.cpp
    Font/PathFontProvider.cpp
    Font/Typeface.cpp
    Font/TypefaceSkia.cpp
    Font/WOFF/Loader.cpp
    Font/WOFF2/Loader.cpp
    FontCascadeList.cpp
    GradientPainting.cpp
    Matrix4x4.cpp
    ImageFormats/BMPWriter.cpp
    ImageFormats/JPEGWriter.cpp
    ImageFormats/PNGWriter.cpp
    ImageFormats/WebPSharedLossless.cpp
    ImageFormats/WebPWriter.cpp
    ImageFormats/WebPWriterLossless.cpp
    DecodedImageFrameSkiaImageCache.cpp
    PaintStyle.cpp
    Painter.cpp
    PainterSkia.cpp
    PaintingSurface.cpp
    Palette.cpp
    Path.cpp
    PathSkia.cpp
    Point.cpp
    Rect.cpp
    SharedImage.cpp
    SharedImageBuffer.cpp
    ShareableBitmap.cpp
    ShapeFeature.cpp
    Size.cpp
    SkiaBackendContext.cpp
    SkiaUtils.cpp
    SystemTheme.cpp
    TextLayout.cpp
    Triangle.cpp
    VectorGraphic.cpp
    YUVData.cpp
)

if (APPLE)
    list(APPEND SOURCES MetalContext.mm)
endif()

if (HAS_VULKAN)
    list(APPEND SOURCES VulkanContext.cpp VulkanImage.cpp)
endif()

if (HAS_FONTCONFIG)
    list(APPEND SOURCES Font/GlobalFontConfig.cpp)
endif()

ladybird_lib(LibGfx gfx)

target_link_libraries(LibGfx PRIVATE LibCompress LibCore LibCrypto LibFileSystem LibTextCodec LibIPC LibSync LibUnicode)

target_link_libraries(LibGfx PRIVATE PkgConfig::WOFF2 JPEG::JPEG PNG::PNG skia harfbuzz)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libgfx_rust FFI_HEADER RustFFI.h)
target_link_libraries(LibGfx PRIVATE libgfx_rust)

if (HAS_FONTCONFIG)
    target_link_libraries(LibGfx PRIVATE Fontconfig::Fontconfig)
endif()

if (NOT ANDROID)
    # FIXME: Build vcpkg libraries with clang-cl so they can also build in sanitize.
    # https://github.com/LadybirdBrowser/ladybird/issues/5224
    if (WIN32 AND ENABLE_ADDRESS_SANITIZER)
        # We have to disable ASAN for the full target as fixing the issue in the initial SkiaBackendContext.cpp TU
        #  alone causes transitive issues in other TUs that depend on it
        target_compile_options(LibGfx PRIVATE -fno-sanitize=address)
    endif()
endif()

if (APPLE)
    target_link_libraries(LibCore PUBLIC "-framework Metal")
    target_link_libraries(LibCore PUBLIC "-framework Accelerate")
endif()

if (HAS_VULKAN)
    target_link_libraries(LibCore PUBLIC Vulkan::Vulkan Vulkan::Headers)

    if (USE_VULKAN_DMABUF_IMAGES)
        pkg_check_modules(LibDRM REQUIRED libdrm)
        target_include_directories(LibGfx PUBLIC ${LibDRM_INCLUDE_DIRS})
    endif()
endif()
