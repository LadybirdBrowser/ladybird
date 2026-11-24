/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/Math.h>
#include <AK/OwnPtr.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/VideoFrameSkia.h>
#include <LibCore/IOSurface.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/MetalContext.h>
#include <LibGfx/VideoSurfaceImage.h>
#include <LibMediaCodecs/VideoSurface.h>
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
    return SkImages::TextureFromYUVAPixmaps(gr_context, Compositor::make_yuva_pixmaps(yuv_data), skgpu::Mipmapped::kNo, false, SkColorSpace::MakeSRGB());
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

    auto image = Compositor::sk_image_from_video_surface(MUST(Media::VideoSurface::create(surface.release_value())), {}, *context);
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

    auto image = Compositor::sk_image_from_video_surface(MUST(Media::VideoSurface::create(surface.release_value())), {}, *context);
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

    auto surface = MUST(Media::VideoSurface::create(Core::IOSurfaceHandle::create(16, 16)));
    EXPECT(Compositor::sk_image_from_video_surface(surface, {}, *context) == nullptr);
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

    auto surface_image = Compositor::sk_image_from_video_surface(MUST(Media::VideoSurface::create(surface.release_value())), {}, *context);
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

namespace {

// A '420v' surface described plane by plane, so a test can give it the geometry another process could.
Optional<Core::IOSurfaceHandle> create_surface_with_chroma_height(int width, int height, int chroma_height)
{
    auto set_number = [](CFMutableDictionaryRef dictionary, CFStringRef key, long value) {
        auto* number = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongType, &value);
        CFDictionarySetValue(dictionary, key, number);
        CFRelease(number);
    };
    auto create_dictionary = [] {
        return CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    };
    auto create_plane = [&](long plane_width, long plane_height, long bytes_per_element, long offset) {
        auto* plane = create_dictionary();
        set_number(plane, kIOSurfacePlaneWidth, plane_width);
        set_number(plane, kIOSurfacePlaneHeight, plane_height);
        set_number(plane, kIOSurfacePlaneBytesPerElement, bytes_per_element);
        set_number(plane, kIOSurfacePlaneBytesPerRow, plane_width * bytes_per_element);
        set_number(plane, kIOSurfacePlaneOffset, offset);
        set_number(plane, kIOSurfacePlaneSize, plane_width * bytes_per_element * plane_height);
        return plane;
    };

    long luma_size = static_cast<long>(width) * height;
    CFTypeRef planes[] = { create_plane(width, height, 1, 0), create_plane(width / 2, chroma_height, 2, luma_size) };
    auto* plane_array = CFArrayCreate(kCFAllocatorDefault, planes, 2, &kCFTypeArrayCallBacks);
    CFRelease(planes[0]);
    CFRelease(planes[1]);

    auto* properties = create_dictionary();
    set_number(properties, kIOSurfaceWidth, width);
    set_number(properties, kIOSurfaceHeight, height);
    set_number(properties, kIOSurfacePixelFormat, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    set_number(properties, kIOSurfaceAllocSize, luma_size + (static_cast<long>(width) * chroma_height));
    CFDictionarySetValue(properties, kIOSurfacePlaneInfo, plane_array);
    CFRelease(plane_array);

    auto* io_surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    if (io_surface == nullptr)
        return {};
    auto handle = Core::IOSurfaceHandle::from_ref(io_surface);
    CFRelease(io_surface);
    return handle;
}

}

TEST_CASE(accepts_surfaces_laid_out_like_a_decoders)
{
    EXPECT(!Gfx::validate_biplanar_video_surface(create_decoder_shaped_surface(63, 47, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange).release_value()).is_error());
    EXPECT(!Gfx::validate_biplanar_video_surface(create_decoder_shaped_surface(64, 48, kCVPixelFormatType_420YpCbCr10BiPlanarFullRange).release_value()).is_error());
    EXPECT(!Gfx::validate_biplanar_video_surface(create_surface_with_chroma_height(64, 48, 24).release_value()).is_error());
}

// Surfaces arrive from the decoder's process, so their planes must hold what their format says before anything reads
// them, or the readers walk past the chroma plane.
TEST_CASE(refuses_surfaces_whose_planes_do_not_hold_their_format)
{
    EXPECT(Gfx::validate_biplanar_video_surface(Core::IOSurfaceHandle::create(16, 16)).is_error());

    auto short_chroma = create_surface_with_chroma_height(64, 48, 1);
    VERIFY(short_chroma.has_value());
    EXPECT(Gfx::validate_biplanar_video_surface(*short_chroma).is_error());
}

TEST_CASE(biplanar_conversion_refuses_planes_shorter_than_the_size)
{
    auto luma = MUST(ByteBuffer::create_zeroed(64 * 48));
    auto chroma = MUST(ByteBuffer::create_zeroed(64 * 24));
    Gfx::BiplanarYUVPlane luma_plane { .data = luma.bytes(), .stride = 64 };
    EXPECT(!Gfx::biplanar_yuv_to_bitmap({ 64, 48 }, 8, Media::Subsampling::yuv420(), {}, luma_plane, { .data = chroma.bytes(), .stride = 64 }).is_error());
    EXPECT(Gfx::biplanar_yuv_to_bitmap({ 64, 48 }, 8, Media::Subsampling::yuv420(), {}, luma_plane, { .data = chroma.bytes().slice(0, 64), .stride = 64 }).is_error());
    EXPECT(Gfx::biplanar_yuv_to_bitmap({ 64, 48 }, 8, Media::Subsampling::yuv420(), {}, luma_plane, { .data = chroma.bytes(), .stride = 16 }).is_error());
}

namespace {

// Green, blue and red, held where luma, blue chroma and red chroma would be.
constexpr Sample RGB_TEST_SAMPLE { 40, 200, 120 };
constexpr Gfx::Color RGB_TEST_COLOR { 120, 40, 200 };
constexpr Sample TEN_BIT_RGB_TEST_SAMPLE { 160, 800, 480 };
constexpr Media::CodingIndependentCodePoints RGB_CICP { Media::ColorPrimaries::Unspecified, Media::TransferCharacteristics::Unspecified, Media::MatrixCoefficients::Identity, Media::VideoFullRangeFlag::Full };

bool colors_are_close(Gfx::Color a, Gfx::Color b)
{
    auto close = [](u8 x, u8 y) { return AK::abs(static_cast<int>(x) - static_cast<int>(y)) <= 2; };
    return close(a.red(), b.red()) && close(a.green(), b.green()) && close(a.blue(), b.blue());
}

// Every component of the pattern is constant within square blocks of this many pixels, and differs between them. At a
// block's center, chroma upsampling agrees whichever way it filters, but chroma read from the wrong row or column, or
// with the wrong subsampling, comes from another block.
constexpr int PATTERN_BLOCK_SIZE = 8;

Sample pattern_sample_at(int x, int y, u8 bit_depth)
{
    auto block_x = x / PATTERN_BLOCK_SIZE;
    auto block_y = y / PATTERN_BLOCK_SIZE;
    auto scale = 1 << (bit_depth - 8);
    return {
        static_cast<u16>((96 + (block_x * 4) + (block_y * 8)) * scale),
        static_cast<u16>((64 + (block_x * 16)) * scale),
        static_cast<u16>((80 + (block_y * 16)) * scale),
    };
}

void fill_biplanar_surface_with_pattern(Core::IOSurfaceHandle const& handle)
{
    auto format = Gfx::biplanar_pixel_format(handle.pixel_format());
    VERIFY(format.has_value());
    auto* io_surface = static_cast<IOSurfaceRef>(handle.core_foundation_pointer());
    IOSurfaceLock(io_surface, 0, nullptr);

    // Samples deeper than eight bits are carried in the high bits of each sixteen.
    auto bit_depth = format->bit_depth;
    auto store = [&](u8* row, size_t index, u16 sample) {
        if (bit_depth <= 8)
            row[index] = static_cast<u8>(sample);
        else
            reinterpret_cast<u16*>(row)[index] = static_cast<u16>(sample << (16 - bit_depth));
    };

    auto* luma = static_cast<u8*>(IOSurfaceGetBaseAddressOfPlane(io_surface, 0));
    auto luma_stride = IOSurfaceGetBytesPerRowOfPlane(io_surface, 0);
    for (size_t row = 0; row < handle.plane_height(0); row++) {
        for (size_t column = 0; column < handle.plane_width(0); column++)
            store(luma + (row * luma_stride), column, pattern_sample_at(column, row, bit_depth).luma);
    }

    auto subsampling = format->subsampling;
    auto* chroma = static_cast<u8*>(IOSurfaceGetBaseAddressOfPlane(io_surface, 1));
    auto chroma_stride = IOSurfaceGetBytesPerRowOfPlane(io_surface, 1);
    for (size_t row = 0; row < handle.plane_height(1); row++) {
        for (size_t column = 0; column < handle.plane_width(1); column++) {
            auto sample = pattern_sample_at(column << subsampling.x(), row << subsampling.y(), bit_depth);
            store(chroma + (row * chroma_stride), column * 2, sample.blue_chroma);
            store(chroma + (row * chroma_stride), (column * 2) + 1, sample.red_chroma);
        }
    }

    IOSurfaceUnlock(io_surface, 0, nullptr);
}

// The pattern laid out the way a software decoder hands it over, as three separate planes.
struct PatternPlanes {
    AK_ALLOC_WITH_KMALLOC;

    ByteBuffer luma;
    ByteBuffer blue_chroma;
    ByteBuffer red_chroma;
    Optional<Gfx::YUVData> data;
};

NonnullOwnPtr<PatternPlanes> pattern_planes(u8 bit_depth, Media::Subsampling subsampling)
{
    auto planes = make<PatternPlanes>();
    size_t bytes_per_sample = bit_depth > 8 ? 2 : 1;
    Gfx::Size<size_t> size { TEST_WIDTH, TEST_HEIGHT };
    auto chroma_size = subsampling.subsampled_size(size);
    planes->luma = MUST(ByteBuffer::create_uninitialized(size.width() * size.height() * bytes_per_sample));
    planes->blue_chroma = MUST(ByteBuffer::create_uninitialized(chroma_size.width() * chroma_size.height() * bytes_per_sample));
    planes->red_chroma = MUST(ByteBuffer::create_uninitialized(chroma_size.width() * chroma_size.height() * bytes_per_sample));

    // Separate planes keep samples deeper than eight bits in the low bits of each sixteen.
    auto store = [&](ByteBuffer& plane, size_t index, u16 sample) {
        if (bytes_per_sample == 1)
            plane[index] = static_cast<u8>(sample);
        else
            reinterpret_cast<u16*>(plane.data())[index] = sample;
    };
    for (size_t row = 0; row < size.height(); row++) {
        for (size_t column = 0; column < size.width(); column++)
            store(planes->luma, (row * size.width()) + column, pattern_sample_at(column, row, bit_depth).luma);
    }
    for (size_t row = 0; row < chroma_size.height(); row++) {
        for (size_t column = 0; column < chroma_size.width(); column++) {
            auto sample = pattern_sample_at(column << subsampling.x(), row << subsampling.y(), bit_depth);
            store(planes->blue_chroma, (row * chroma_size.width()) + column, sample.blue_chroma);
            store(planes->red_chroma, (row * chroma_size.width()) + column, sample.red_chroma);
        }
    }

    planes->data = MUST(Gfx::YUVData::create({ TEST_WIDTH, TEST_HEIGHT }, bit_depth, subsampling, {},
        planes->luma.bytes(), planes->blue_chroma.bytes(), planes->red_chroma.bytes()));
    return planes;
}

Optional<SkBitmap> pixels_of(sk_sp<SkImage> const& image, GrDirectContext* gr_context)
{
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(image->width(), image->height(), SkColorSpace::MakeSRGB())))
        return {};
    if (!image->readPixels(gr_context, bitmap.pixmap(), 0, 0))
        return {};
    return bitmap;
}

// Compares the center of every block of the pattern.
void expect_pattern_blocks_alike(Function<Gfx::Color(int, int)> const& actual_color_at, Function<Gfx::Color(int, int)> const& expected_color_at)
{
    for (int block_y = 0; block_y < TEST_HEIGHT / PATTERN_BLOCK_SIZE; block_y++) {
        for (int block_x = 0; block_x < TEST_WIDTH / PATTERN_BLOCK_SIZE; block_x++) {
            auto x = (block_x * PATTERN_BLOCK_SIZE) + (PATTERN_BLOCK_SIZE / 2);
            auto y = (block_y * PATTERN_BLOCK_SIZE) + (PATTERN_BLOCK_SIZE / 2);
            auto actual = actual_color_at(x, y);
            auto expected = expected_color_at(x, y);
            if (!colors_are_close(actual, expected))
                warnln("Block ({}, {}): got {}, expected {}", block_x, block_y, actual, expected);
            EXPECT(colors_are_close(actual, expected));
        }
    }
}

// Reads a surface back on the CPU and checks it against its separate planes converted to pixels.
void expect_surface_reads_back_as_its_pattern(OSType pixel_format)
{
    auto surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, pixel_format);
    VERIFY(surface.has_value());
    fill_biplanar_surface_with_pattern(*surface);
    auto format = Gfx::biplanar_pixel_format(pixel_format).value();

    auto surface_bitmap = MUST(Gfx::bitmap_from_video_surface(*surface, {}));
    auto planes = pattern_planes(format.bit_depth, format.subsampling);
    auto planes_bitmap = MUST(planes->data->to_bitmap());
    auto color_at = [](Gfx::Bitmap const& bitmap) {
        return Function<Gfx::Color(int, int)>([&bitmap](int x, int y) { return bitmap.get_pixel(x, y); });
    };
    expect_pattern_blocks_alike(color_at(*surface_bitmap), color_at(*planes_bitmap));
}

// Draws a surface on the GPU and checks it against its separate planes drawn the same way.
void expect_surface_renders_as_its_pattern(OSType pixel_format, Gfx::SkiaBackendContext& context)
{
    auto* gr_context = context.sk_context();
    auto surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, pixel_format);
    VERIFY(surface.has_value());
    fill_biplanar_surface_with_pattern(*surface);
    auto format = Gfx::biplanar_pixel_format(pixel_format).value();

    auto surface_image = Compositor::sk_image_from_video_surface(MUST(Media::VideoSurface::create(surface.release_value())), {}, context);
    VERIFY(surface_image != nullptr);
    auto surface_pixels = pixels_of(surface_image, gr_context);
    VERIFY(surface_pixels.has_value());

    auto planes = pattern_planes(format.bit_depth, format.subsampling);
    auto planes_image = SkImages::TextureFromYUVAPixmaps(gr_context, Compositor::make_yuva_pixmaps(*planes->data), skgpu::Mipmapped::kNo, false, SkColorSpace::MakeSRGB());
    VERIFY(planes_image != nullptr);
    auto planes_pixels = pixels_of(planes_image, gr_context);
    VERIFY(planes_pixels.has_value());

    auto color_at = [](SkBitmap const& bitmap) {
        return Function<Gfx::Color(int, int)>([&bitmap](int x, int y) {
            auto color = bitmap.getColor(x, y);
            return Gfx::Color(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
        });
    };
    expect_pattern_blocks_alike(color_at(*surface_pixels), color_at(*planes_pixels));
}

}

TEST_CASE(accepts_four_four_four_surfaces_laid_out_like_a_decoders)
{
    EXPECT(!Gfx::validate_biplanar_video_surface(create_decoder_shaped_surface(63, 47, kCVPixelFormatType_444YpCbCr8BiPlanarFullRange).release_value()).is_error());
    EXPECT(!Gfx::validate_biplanar_video_surface(create_decoder_shaped_surface(64, 48, kCVPixelFormatType_444YpCbCr10BiPlanarFullRange).release_value()).is_error());
}

TEST_CASE(reads_back_a_four_four_four_surface_as_the_same_pixels_its_separate_planes_convert_to)
{
    expect_surface_reads_back_as_its_pattern(kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange);
    expect_surface_reads_back_as_its_pattern(kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange);
}

TEST_CASE(reads_back_an_rgb_surface_as_its_green_blue_and_red_planes)
{
    auto surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, kCVPixelFormatType_444YpCbCr8BiPlanarFullRange);
    VERIFY(surface.has_value());
    fill_biplanar_surface(*surface, RGB_TEST_SAMPLE);

    auto bitmap = MUST(Gfx::bitmap_from_video_surface(*surface, RGB_CICP));
    EXPECT_EQ(bitmap->get_pixel(TEST_WIDTH / 2, TEST_HEIGHT / 2), RGB_TEST_COLOR);
}

TEST_CASE(refuses_to_read_back_a_subsampled_surface_as_rgb)
{
    auto surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
    VERIFY(surface.has_value());
    EXPECT(Gfx::bitmap_from_video_surface(*surface, RGB_CICP).is_error());
}

TEST_CASE(a_four_four_four_surface_and_separate_planes_of_the_same_samples_render_alike)
{
    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }
    expect_surface_renders_as_its_pattern(kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange, *context);
    expect_surface_renders_as_its_pattern(kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange, *context);
}

TEST_CASE(a_four_two_two_surface_and_separate_planes_of_the_same_samples_render_alike)
{
    EXPECT(!Gfx::validate_biplanar_video_surface(create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange).release_value()).is_error());
    expect_surface_reads_back_as_its_pattern(kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange);
    expect_surface_reads_back_as_its_pattern(kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange);

    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }
    expect_surface_renders_as_its_pattern(kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange, *context);
    expect_surface_renders_as_its_pattern(kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange, *context);
}

TEST_CASE(an_rgb_surface_renders_as_its_green_blue_and_red_planes)
{
    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }
    auto* gr_context = context->sk_context();

    auto surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, kCVPixelFormatType_444YpCbCr8BiPlanarFullRange);
    VERIFY(surface.has_value());
    fill_biplanar_surface(*surface, RGB_TEST_SAMPLE);
    auto image = Compositor::sk_image_from_video_surface(MUST(Media::VideoSurface::create(surface.release_value())), RGB_CICP, *context);
    VERIFY(image != nullptr);
    auto color = center_pixel_of(image, gr_context);
    VERIFY(color.has_value());
    outln("rgb surface: {} {} {}", color->red(), color->green(), color->blue());
    EXPECT(colors_are_close(*color, RGB_TEST_COLOR));
}

TEST_CASE(a_ten_bit_rgb_surface_renders_as_its_green_blue_and_red_planes)
{
    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }
    auto* gr_context = context->sk_context();

    auto surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, kCVPixelFormatType_444YpCbCr10BiPlanarFullRange);
    VERIFY(surface.has_value());
    fill_ten_bit_biplanar_surface(*surface, TEN_BIT_RGB_TEST_SAMPLE);
    auto image = Compositor::sk_image_from_video_surface(MUST(Media::VideoSurface::create(surface.release_value())), RGB_CICP, *context);
    VERIFY(image != nullptr);
    auto color = center_pixel_of(image, gr_context);
    VERIFY(color.has_value());
    outln("ten-bit rgb surface: {} {} {}", color->red(), color->green(), color->blue());
    EXPECT(colors_are_close(*color, RGB_TEST_COLOR));
}

TEST_CASE(separate_rgb_planes_render_as_green_blue_and_red)
{
    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }
    auto* gr_context = context->sk_context();

    auto green_plane = MUST(ByteBuffer::create_uninitialized(TEST_WIDTH * TEST_HEIGHT));
    auto blue_plane = MUST(ByteBuffer::create_uninitialized(TEST_WIDTH * TEST_HEIGHT));
    auto red_plane = MUST(ByteBuffer::create_uninitialized(TEST_WIDTH * TEST_HEIGHT));
    green_plane.bytes().fill(static_cast<u8>(RGB_TEST_SAMPLE.luma));
    blue_plane.bytes().fill(static_cast<u8>(RGB_TEST_SAMPLE.blue_chroma));
    red_plane.bytes().fill(static_cast<u8>(RGB_TEST_SAMPLE.red_chroma));
    auto yuv_data = MUST(Gfx::YUVData::create({ TEST_WIDTH, TEST_HEIGHT }, 8, Media::Subsampling::yuv444(), RGB_CICP,
        green_plane.bytes(), blue_plane.bytes(), red_plane.bytes()));

    auto image = SkImages::TextureFromYUVAPixmaps(gr_context, Compositor::make_yuva_pixmaps(yuv_data), skgpu::Mipmapped::kNo, false, SkColorSpace::MakeSRGB());
    VERIFY(image != nullptr);
    auto color = center_pixel_of(image, gr_context);
    VERIFY(color.has_value());
    EXPECT(colors_are_close(*color, RGB_TEST_COLOR));
    EXPECT_EQ(MUST(yuv_data.to_bitmap())->get_pixel(TEST_WIDTH / 2, TEST_HEIGHT / 2), RGB_TEST_COLOR);
}

TEST_CASE(separate_ten_bit_rgb_planes_render_as_green_blue_and_red)
{
    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }
    auto* gr_context = context->sk_context();

    auto make_plane = [](u16 value) {
        auto plane = MUST(ByteBuffer::create_uninitialized(TEST_WIDTH * TEST_HEIGHT * sizeof(u16)));
        auto* samples = reinterpret_cast<u16*>(plane.data());
        for (size_t index = 0; index < plane.size() / sizeof(u16); index++)
            samples[index] = value;
        return plane;
    };
    auto green_plane = make_plane(TEN_BIT_RGB_TEST_SAMPLE.luma);
    auto blue_plane = make_plane(TEN_BIT_RGB_TEST_SAMPLE.blue_chroma);
    auto red_plane = make_plane(TEN_BIT_RGB_TEST_SAMPLE.red_chroma);
    auto yuv_data = MUST(Gfx::YUVData::create({ TEST_WIDTH, TEST_HEIGHT }, 10, Media::Subsampling::yuv444(), RGB_CICP,
        green_plane.bytes(), blue_plane.bytes(), red_plane.bytes()));

    auto image = SkImages::TextureFromYUVAPixmaps(gr_context, Compositor::make_yuva_pixmaps(yuv_data), skgpu::Mipmapped::kNo, false, SkColorSpace::MakeSRGB());
    VERIFY(image != nullptr);
    auto color = center_pixel_of(image, gr_context);
    VERIFY(color.has_value());
    EXPECT(colors_are_close(*color, RGB_TEST_COLOR));
}

// Studio range RGB spans 16 to 235 in every component.
TEST_CASE(studio_range_rgb_is_stretched_to_full_range)
{
    constexpr Sample studio_sample { 16, 235, 126 };
    constexpr Sample ten_bit_studio_sample { 64, 940, 504 };
    constexpr Gfx::Color expected_color { 128, 0, 255 };
    auto studio_cicp = RGB_CICP;
    studio_cicp.set_video_full_range_flag(Media::VideoFullRangeFlag::Studio);

    auto surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange);
    VERIFY(surface.has_value());
    fill_biplanar_surface(*surface, studio_sample);
    EXPECT_EQ(MUST(Gfx::bitmap_from_video_surface(*surface, studio_cicp))->get_pixel(TEST_WIDTH / 2, TEST_HEIGHT / 2), expected_color);

    auto ten_bit_surface = create_decoder_shaped_surface(TEST_WIDTH, TEST_HEIGHT, kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange);
    VERIFY(ten_bit_surface.has_value());
    fill_ten_bit_biplanar_surface(*ten_bit_surface, ten_bit_studio_sample);
    EXPECT_EQ(MUST(Gfx::bitmap_from_video_surface(*ten_bit_surface, studio_cicp))->get_pixel(TEST_WIDTH / 2, TEST_HEIGHT / 2), expected_color);

    auto green_plane = MUST(ByteBuffer::create_uninitialized(TEST_WIDTH * TEST_HEIGHT));
    auto blue_plane = MUST(ByteBuffer::create_uninitialized(TEST_WIDTH * TEST_HEIGHT));
    auto red_plane = MUST(ByteBuffer::create_uninitialized(TEST_WIDTH * TEST_HEIGHT));
    green_plane.bytes().fill(static_cast<u8>(studio_sample.luma));
    blue_plane.bytes().fill(static_cast<u8>(studio_sample.blue_chroma));
    red_plane.bytes().fill(static_cast<u8>(studio_sample.red_chroma));
    auto yuv_data = MUST(Gfx::YUVData::create({ TEST_WIDTH, TEST_HEIGHT }, 8, Media::Subsampling::yuv444(), studio_cicp,
        green_plane.bytes(), blue_plane.bytes(), red_plane.bytes()));
    EXPECT_EQ(MUST(yuv_data.to_bitmap())->get_pixel(TEST_WIDTH / 2, TEST_HEIGHT / 2), expected_color);

    auto context = gpu_context();
    if (!context) {
        warnln("No GPU context available, skipping");
        return;
    }
    auto* gr_context = context->sk_context();

    auto surface_image = Compositor::sk_image_from_video_surface(MUST(Media::VideoSurface::create(surface.release_value())), studio_cicp, *context);
    VERIFY(surface_image != nullptr);
    auto surface_color = center_pixel_of(surface_image, gr_context);
    VERIFY(surface_color.has_value());
    EXPECT(colors_are_close(*surface_color, expected_color));

    auto planes_image = SkImages::TextureFromYUVAPixmaps(gr_context, Compositor::make_yuva_pixmaps(yuv_data), skgpu::Mipmapped::kNo, false, SkColorSpace::MakeSRGB());
    VERIFY(planes_image != nullptr);
    auto planes_color = center_pixel_of(planes_image, gr_context);
    VERIFY(planes_color.has_value());
    EXPECT(colors_are_close(*planes_color, expected_color));
}
