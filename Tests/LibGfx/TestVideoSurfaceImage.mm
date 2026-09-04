/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Math.h>
#include <LibCore/IOSurface.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/MetalContext.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/VideoSurfaceImage.h>
#include <LibTest/TestCase.h>

#include <LibGfx/YUVData.h>

#include <core/SkBitmap.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkYUVAPixmaps.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>

namespace {

RefPtr<Gfx::SkiaBackendContext> gpu_context()
{
    auto metal_context = Gfx::get_metal_context();
    if (!metal_context)
        return nullptr;
    return Gfx::SkiaBackendContext::create_metal_context(metal_context.release_nonnull());
}

// A surface laid out the way a hardware decoder hands one back, rather than the single RGBA plane
// Core::IOSurfaceHandle::create() allocates.
Optional<Core::IOSurfaceHandle> create_decoder_shaped_surface(int width, int height, OSType pixel_format)
{
    auto* io_surface_properties = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto* attributes = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, io_surface_properties);

    CVPixelBufferRef pixel_buffer = nullptr;
    auto status = CVPixelBufferCreate(kCFAllocatorDefault, width, height, pixel_format, attributes, &pixel_buffer);
    CFRelease(attributes);
    CFRelease(io_surface_properties);
    if (status != kCVReturnSuccess || pixel_buffer == nullptr)
        return {};

    auto* io_surface = CVPixelBufferGetIOSurface(pixel_buffer);
    if (io_surface == nullptr) {
        CVPixelBufferRelease(pixel_buffer);
        return {};
    }

    auto handle = Core::IOSurfaceHandle::from_ref(io_surface);
    CVPixelBufferRelease(pixel_buffer);
    return handle;
}

struct Sample {
    u16 luma;
    u16 blue_chroma;
    u16 red_chroma;
};

constexpr Sample TEST_SAMPLE { 128, 64, 192 };
constexpr int TEST_WIDTH = 64;
constexpr int TEST_HEIGHT = 48;

void fill_biplanar_surface(Core::IOSurfaceHandle const& handle, Sample sample)
{
    auto* io_surface = static_cast<IOSurfaceRef>(handle.core_foundation_pointer());
    IOSurfaceLock(io_surface, 0, nullptr);

    auto* luma = static_cast<u8*>(IOSurfaceGetBaseAddressOfPlane(io_surface, 0));
    auto luma_stride = IOSurfaceGetBytesPerRowOfPlane(io_surface, 0);
    for (size_t row = 0; row < handle.plane_height(0); row++)
        memset(luma + row * luma_stride, sample.luma, handle.plane_width(0));

    auto* chroma = static_cast<u8*>(IOSurfaceGetBaseAddressOfPlane(io_surface, 1));
    auto chroma_stride = IOSurfaceGetBytesPerRowOfPlane(io_surface, 1);
    for (size_t row = 0; row < handle.plane_height(1); row++) {
        auto* chroma_row = chroma + row * chroma_stride;
        for (size_t column = 0; column < handle.plane_width(1); column++) {
            chroma_row[column * 2] = static_cast<u8>(sample.blue_chroma);
            chroma_row[column * 2 + 1] = static_cast<u8>(sample.red_chroma);
        }
    }

    IOSurfaceUnlock(io_surface, 0, nullptr);
}

constexpr Sample TEN_BIT_TEST_SAMPLE { 512, 256, 768 };

void fill_ten_bit_biplanar_surface(Core::IOSurfaceHandle const& handle, Sample sample)
{
    auto* io_surface = static_cast<IOSurfaceRef>(handle.core_foundation_pointer());
    IOSurfaceLock(io_surface, 0, nullptr);

    // Ten-bit samples are carried in the high bits of each sixteen, so the low six are left clear.
    auto to_component = [](u16 sample) { return static_cast<u16>(sample << 6); };

    auto* luma = static_cast<u8*>(IOSurfaceGetBaseAddressOfPlane(io_surface, 0));
    auto luma_stride = IOSurfaceGetBytesPerRowOfPlane(io_surface, 0);
    for (size_t row = 0; row < handle.plane_height(0); row++) {
        auto* luma_row = reinterpret_cast<u16*>(luma + row * luma_stride);
        for (size_t column = 0; column < handle.plane_width(0); column++)
            luma_row[column] = to_component(sample.luma);
    }

    auto* chroma = static_cast<u8*>(IOSurfaceGetBaseAddressOfPlane(io_surface, 1));
    auto chroma_stride = IOSurfaceGetBytesPerRowOfPlane(io_surface, 1);
    for (size_t row = 0; row < handle.plane_height(1); row++) {
        auto* chroma_row = reinterpret_cast<u16*>(chroma + row * chroma_stride);
        for (size_t column = 0; column < handle.plane_width(1); column++) {
            chroma_row[column * 2] = to_component(sample.blue_chroma);
            chroma_row[column * 2 + 1] = to_component(sample.red_chroma);
        }
    }

    IOSurfaceUnlock(io_surface, 0, nullptr);
}

// The same samples laid out the way a software decoder hands them over, as three separate planes.
sk_sp<SkImage> reference_image_from_planes(Sample sample, GrDirectContext* gr_context)
{
    auto luma_plane = MUST(ByteBuffer::create_uninitialized(TEST_WIDTH * TEST_HEIGHT));
    auto blue_chroma_plane = MUST(ByteBuffer::create_uninitialized((TEST_WIDTH / 2) * (TEST_HEIGHT / 2)));
    auto red_chroma_plane = MUST(ByteBuffer::create_uninitialized((TEST_WIDTH / 2) * (TEST_HEIGHT / 2)));
    luma_plane.bytes().fill(static_cast<u8>(sample.luma));
    blue_chroma_plane.bytes().fill(static_cast<u8>(sample.blue_chroma));
    red_chroma_plane.bytes().fill(static_cast<u8>(sample.red_chroma));

    auto yuv_data = MUST(Gfx::YUVData::create({ TEST_WIDTH, TEST_HEIGHT }, 8, Media::Subsampling::yuv420(), {},
        luma_plane.bytes(), blue_chroma_plane.bytes(), red_chroma_plane.bytes()));
    return SkImages::TextureFromYUVAPixmaps(gr_context, yuv_data.make_pixmaps(), skgpu::Mipmapped::kNo, false, SkColorSpace::MakeSRGB());
}

NonnullRefPtr<Gfx::Bitmap> reference_bitmap_from_planes(Sample sample)
{
    auto luma_plane = MUST(ByteBuffer::create_uninitialized(TEST_WIDTH * TEST_HEIGHT));
    auto blue_chroma_plane = MUST(ByteBuffer::create_uninitialized((TEST_WIDTH / 2) * (TEST_HEIGHT / 2)));
    auto red_chroma_plane = MUST(ByteBuffer::create_uninitialized((TEST_WIDTH / 2) * (TEST_HEIGHT / 2)));
    luma_plane.bytes().fill(static_cast<u8>(sample.luma));
    blue_chroma_plane.bytes().fill(static_cast<u8>(sample.blue_chroma));
    red_chroma_plane.bytes().fill(static_cast<u8>(sample.red_chroma));

    auto yuv_data = MUST(Gfx::YUVData::create({ TEST_WIDTH, TEST_HEIGHT }, 8, Media::Subsampling::yuv420(), {},
        luma_plane.bytes(), blue_chroma_plane.bytes(), red_chroma_plane.bytes()));
    return MUST(yuv_data.to_bitmap());
}

NonnullRefPtr<Gfx::Bitmap> reference_bitmap_from_ten_bit_planes(Sample sample)
{
    auto fill = [](Bytes plane, u16 value) {
        auto* samples = reinterpret_cast<u16*>(plane.data());
        for (size_t index = 0; index < plane.size() / sizeof(u16); index++)
            samples[index] = value;
    };

    auto luma_plane = MUST(ByteBuffer::create_uninitialized(TEST_WIDTH * TEST_HEIGHT * sizeof(u16)));
    auto blue_chroma_plane = MUST(ByteBuffer::create_uninitialized((TEST_WIDTH / 2) * (TEST_HEIGHT / 2) * sizeof(u16)));
    auto red_chroma_plane = MUST(ByteBuffer::create_uninitialized((TEST_WIDTH / 2) * (TEST_HEIGHT / 2) * sizeof(u16)));
    fill(luma_plane.bytes(), sample.luma);
    fill(blue_chroma_plane.bytes(), sample.blue_chroma);
    fill(red_chroma_plane.bytes(), sample.red_chroma);

    auto yuv_data = MUST(Gfx::YUVData::create({ TEST_WIDTH, TEST_HEIGHT }, 10, Media::Subsampling::yuv420(), {},
        luma_plane.bytes(), blue_chroma_plane.bytes(), red_chroma_plane.bytes()));
    return MUST(yuv_data.to_bitmap());
}

Optional<Gfx::Color> center_pixel_of(sk_sp<SkImage> const& image, GrDirectContext* gr_context)
{
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(image->width(), image->height(), SkColorSpace::MakeSRGB())))
        return {};
    if (!image->readPixels(gr_context, bitmap.pixmap(), 0, 0))
        return {};
    auto color = bitmap.getColor(image->width() / 2, image->height() / 2);
    return Gfx::Color(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
}

}

TEST_CASE(imports_an_eight_bit_surface_as_a_gpu_image)
{
    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }

    auto surface = create_decoder_shaped_surface(64, 48, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    VERIFY(surface.has_value());

    auto image = Gfx::sk_image_from_video_surface(*surface, {}, *context);
    EXPECT(image != nullptr);
    EXPECT_EQ(image->width(), 64);
    EXPECT_EQ(image->height(), 48);
}

TEST_CASE(imports_a_ten_bit_surface_as_a_gpu_image)
{
    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }

    auto surface = create_decoder_shaped_surface(64, 48, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);
    VERIFY(surface.has_value());

    auto image = Gfx::sk_image_from_video_surface(*surface, {}, *context);
    EXPECT(image != nullptr);
    EXPECT_EQ(image->width(), 64);
    EXPECT_EQ(image->height(), 48);
}

TEST_CASE(refuses_a_surface_that_holds_no_video_planes)
{
    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }

    auto surface = Core::IOSurfaceHandle::create(16, 16);
    EXPECT(Gfx::sk_image_from_video_surface(surface, {}, *context) == nullptr);
}

TEST_CASE(a_surface_and_separate_planes_of_the_same_samples_render_alike)
{
    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }
    auto* gr_context = context->sk_context();

    auto surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    VERIFY(surface.has_value());
    fill_biplanar_surface(*surface, TEST_SAMPLE);

    auto surface_image = Gfx::sk_image_from_video_surface(*surface, {}, *context);
    VERIFY(surface_image != nullptr);
    auto surface_color = center_pixel_of(surface_image, gr_context);
    VERIFY(surface_color.has_value());

    auto reference = reference_image_from_planes(TEST_SAMPLE, gr_context);
    VERIFY(reference != nullptr);
    auto reference_color = center_pixel_of(reference, gr_context);
    VERIFY(reference_color.has_value());

    outln("surface path: {} {} {}", surface_color->red(), surface_color->green(), surface_color->blue());
    outln("plane path:   {} {} {}", reference_color->red(), reference_color->green(), reference_color->blue());

    EXPECT(AK::abs(static_cast<int>(surface_color->red()) - static_cast<int>(reference_color->red())) <= 2);
    EXPECT(AK::abs(static_cast<int>(surface_color->green()) - static_cast<int>(reference_color->green())) <= 2);
    EXPECT(AK::abs(static_cast<int>(surface_color->blue()) - static_cast<int>(reference_color->blue())) <= 2);
}

TEST_CASE(reads_back_a_surface_as_the_same_pixels_its_separate_planes_convert_to)
{
    auto surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    VERIFY(surface.has_value());
    fill_biplanar_surface(*surface, TEST_SAMPLE);

    auto surface_bitmap = MUST(Gfx::bitmap_from_video_surface(*surface, {}));
    auto reference_bitmap = reference_bitmap_from_planes(TEST_SAMPLE);

    EXPECT_EQ(surface_bitmap->size(), reference_bitmap->size());
    EXPECT_EQ(surface_bitmap->get_pixel(TEST_WIDTH / 2, TEST_HEIGHT / 2), reference_bitmap->get_pixel(TEST_WIDTH / 2, TEST_HEIGHT / 2));
}

TEST_CASE(reads_back_a_ten_bit_surface_as_the_same_pixels_its_separate_planes_convert_to)
{
    auto surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);
    VERIFY(surface.has_value());
    fill_ten_bit_biplanar_surface(*surface, TEN_BIT_TEST_SAMPLE);

    auto surface_bitmap = MUST(Gfx::bitmap_from_video_surface(*surface, {}));
    auto reference_bitmap = reference_bitmap_from_ten_bit_planes(TEN_BIT_TEST_SAMPLE);

    EXPECT_EQ(surface_bitmap->size(), reference_bitmap->size());
    EXPECT_EQ(surface_bitmap->get_pixel(TEST_WIDTH / 2, TEST_HEIGHT / 2), reference_bitmap->get_pixel(TEST_WIDTH / 2, TEST_HEIGHT / 2));
}

TEST_CASE(refuses_to_read_back_a_surface_that_holds_no_video_planes)
{
    auto surface = Core::IOSurfaceHandle::create(16, 16);
    EXPECT(Gfx::bitmap_from_video_surface(surface, {}).is_error());
}
