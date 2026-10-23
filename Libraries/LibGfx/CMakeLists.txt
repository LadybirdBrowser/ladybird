set(SOURCES
    AffineTransform.cpp
    Bitmap.cpp
    BitmapSequence.cpp
    CMYKBitmap.cpp
    Color.cpp
    ColorConversion.cpp
    ColorSpace.cpp
    CornerRadii.cpp
    Cursor.cpp
    Filter.cpp
    Font/FaceDescription.cpp
    Font/Font.cpp
    Font/FontCatalog.cpp
    Font/FontDatabase.cpp
    Font/FontSupport.cpp
    Font/FontTable.cpp
    Font/FontVariationSettings.cpp
    Font/PathFontProvider.cpp
    Font/SharedFontProvider.cpp
    Font/SystemFallbackFonts.cpp
    Font/SystemFontMatcher.cpp
    Font/Typeface.cpp
    Font/WOFF/Loader.cpp
    Font/WOFF2/Loader.cpp
    FontCascadeList.cpp
    GpuDeviceIdentity.cpp
    GradientPainting.cpp
    Gradients.cpp
    LinuxDmaBufHandle.cpp
    Matrix4x4.cpp
    DecodedImageFrame.cpp
    RustProcessState.cpp
    ImageFormats/BMPWriter.cpp
    ImageFormats/JPEGWriter.cpp
    ImageFormats/PNGWriter.cpp
    ImageFormats/WebPSharedLossless.cpp
    ImageFormats/WebPWriter.cpp
    ImageFormats/WebPWriterLossless.cpp
    PaintStyle.cpp
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
    SystemTheme.cpp
    TextLayout.cpp
    YUVData.cpp
)

if (APPLE)
    list(APPEND SOURCES Font/TypefaceCoreText.cpp MetalContext.mm VideoSurfaceImage.mm)
endif()

if (HAS_DIRECTX)
    list(APPEND SOURCES D3DSharedTexture.cpp Direct3DContext.cpp)
endif()

if (HAS_VULKAN)
    list(APPEND SOURCES ImportedYCbCrVulkanImage.cpp VulkanContext.cpp VulkanImage.cpp)
endif()

if (HAS_FONTCONFIG)
    list(APPEND SOURCES Font/GlobalFontConfig.cpp Font/SystemFontMatcherFontconfig.cpp)
elseif (APPLE)
    list(APPEND SOURCES Font/SystemFontMatcherCoreText.cpp)
elseif (WIN32)
    list(APPEND SOURCES Font/SystemFontMatcherDirectWrite.cpp)
else()
    list(APPEND SOURCES Font/SystemFontMatcherNone.cpp)
endif()

compile_ipc(Point.ipc PointCodecs.h)
compile_ipc(Rect.ipc RectCodecs.h)
compile_ipc(Size.ipc SizeCodecs.h)

ladybird_lib(LibGfx gfx)

target_link_libraries(LibGfx PRIVATE LibCompress LibCore LibCrypto LibFileSystem LibTextCodec LibIPC LibUnicode)

target_link_libraries(LibGfx PRIVATE PkgConfig::WOFF2 JPEG::JPEG PNG::PNG skia harfbuzz)

import_rust_crate(
    MANIFEST_PATH Rust/Cargo.toml
    CRATE_NAME libgfx_rust
    FEATURES allocator
    FFI_HEADER RustFFI.h
    KEEP_SYMBOLS ladybird_gfx_register_rust_crate_copy
)
target_link_libraries(LibGfx PRIVATE libgfx_rust)

if (HAS_FONTCONFIG)
    target_link_libraries(LibGfx PRIVATE Fontconfig::Fontconfig)
endif()

# FIXME: Build vcpkg libraries with clang-cl so they can also build in sanitize.
# https://github.com/LadybirdBrowser/ladybird/issues/5224
#   lld-link: error: /failifmismatch: mismatch detected for 'annotate_string':
#   >>> LibGfx.dir\Font\WOFF2\Loader.cpp.obj has value 1
#   >>> woff2dec.lib(woff2_dec.cc.obj) has value 0
# Every TU that includes the standard library carries the value, so the whole target has to go without ASAN.
if (WIN32 AND ENABLE_ADDRESS_SANITIZER)
    target_compile_options(LibGfx PRIVATE -fno-sanitize=address)
endif()

if (APPLE)
    target_link_libraries(LibGfx PRIVATE "-framework Metal" "-framework Accelerate" "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText")
endif()

if (HAS_DIRECTX)
    target_link_libraries(LibGfx PRIVATE d3d12 dxgi d3dcompiler)
endif()

if (WIN32)
    target_link_libraries(LibGfx PRIVATE dwrite)
endif()

if (HAS_VULKAN)
    # Public LibGfx headers include <vulkan/vulkan.h>.
    target_link_libraries(LibGfx PUBLIC Vulkan::Headers)
    target_link_libraries(LibGfx PRIVATE Vulkan::Vulkan)

    if (USE_VULKAN_DMABUF_IMAGES)
        pkg_check_modules(LibDRM REQUIRED libdrm)
        target_include_directories(LibGfx PUBLIC ${LibDRM_INCLUDE_DIRS})
    endif()
endif()
