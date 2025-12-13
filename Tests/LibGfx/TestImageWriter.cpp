/*
 * Copyright (c) 2024, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/BMPLoader.h>
#include <LibGfx/ImageFormats/BMPWriter.h>
#include <LibGfx/ImageFormats/ImageDecoderStream.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <LibGfx/ImageFormats/JPEGWriter.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/ImageFormats/WebPLoader.h>
#include <LibGfx/ImageFormats/WebPWriter.h>
#include <LibTest/TestCase.h>

static ErrorOr<NonnullRefPtr<Gfx::Bitmap>> expect_single_frame(Gfx::ImageDecoderPlugin& plugin_decoder)
{
    EXPECT_EQ(plugin_decoder.frame_count(), 1u);
    EXPECT(!plugin_decoder.is_animated());
    EXPECT(!plugin_decoder.loop_count());

    auto frame_descriptor = TRY(plugin_decoder.frame(0));
    EXPECT_EQ(frame_descriptor.duration, 0);
    return *frame_descriptor.image;
}

static ErrorOr<NonnullRefPtr<Gfx::Bitmap>> expect_single_frame_of_size(Gfx::ImageDecoderPlugin& plugin_decoder, Gfx::IntSize size)
{
    EXPECT_EQ(plugin_decoder.size(), size);
    auto frame = TRY(expect_single_frame(plugin_decoder));
    EXPECT_EQ(frame->size(), size);
    return frame;
}

template<class Writer, class... ExtraArgs>
static ErrorOr<ByteBuffer> encode_bitmap(Gfx::Bitmap const& bitmap, ExtraArgs... extra_args)
{
    if constexpr (requires(AllocatingMemoryStream stream) { Writer::encode(stream, bitmap, extra_args...); }) {
        AllocatingMemoryStream stream;
        TRY(Writer::encode(stream, bitmap, extra_args...));
        return stream.read_until_eof();
    } else {
        return Writer::encode(bitmap, extra_args...);
    }
}

template<class Writer, class Loader>
static ErrorOr<NonnullRefPtr<Gfx::Bitmap>> get_roundtrip_bitmap(Gfx::Bitmap const& bitmap)
{
    auto encoded_data = TRY(encode_bitmap<Writer>(bitmap));
    auto encoded_data_stream = TRY(adopt_nonnull_ref_or_enomem(new Gfx::ImageDecoderStream()));
    encoded_data_stream->append_chunk(move(encoded_data));
    encoded_data_stream->close();
    return expect_single_frame_of_size(*TRY(Loader::create(move(encoded_data_stream))), bitmap.size());
}

static void expect_bitmaps_equal(Gfx::Bitmap const& a, Gfx::Bitmap const& b)
{
    VERIFY(a.size() == b.size());
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            EXPECT_EQ(a.get_pixel(x, y), b.get_pixel(x, y));
}

template<class Writer, class Loader>
static ErrorOr<void> test_roundtrip(Gfx::Bitmap const& bitmap)
{
    auto decoded = TRY((get_roundtrip_bitmap<Writer, Loader>(bitmap)));
    expect_bitmaps_equal(*decoded, bitmap);
    return {};
}

static ErrorOr<AK::NonnullRefPtr<Gfx::Bitmap>> create_test_rgb_bitmap()
{
    auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 47, 33 }));

    for (int y = 0; y < bitmap->height(); ++y)
        for (int x = 0; x < bitmap->width(); ++x)
            bitmap->set_pixel(x, y, Gfx::Color((x * 255) / bitmap->width(), (y * 255) / bitmap->height(), x + y));

    return bitmap;
}

static ErrorOr<AK::NonnullRefPtr<Gfx::Bitmap>> create_test_rgba_bitmap()
{
    auto bitmap = TRY(create_test_rgb_bitmap());

    for (int y = 0; y < bitmap->height(); ++y) {
        for (int x = 0; x < bitmap->width(); ++x) {
            Color pixel = bitmap->get_pixel(x, y);
            pixel.set_alpha(255 - x);
            bitmap->set_pixel(x, y, pixel);
        }
    }

    return bitmap;
}

TEST_CASE(test_bmp)
{
    TRY_OR_FAIL((test_roundtrip<Gfx::BMPWriter, Gfx::BMPImageDecoderPlugin>(TRY_OR_FAIL(create_test_rgb_bitmap()))));
    TRY_OR_FAIL((test_roundtrip<Gfx::BMPWriter, Gfx::BMPImageDecoderPlugin>(TRY_OR_FAIL(create_test_rgba_bitmap()))));
}

TEST_CASE(test_jpeg)
{
    // JPEG is lossy, so the roundtripped bitmap won't match the original bitmap. But it should still have the same size.
    (void)TRY_OR_FAIL((get_roundtrip_bitmap<Gfx::JPEGWriter, Gfx::JPEGImageDecoderPlugin>(TRY_OR_FAIL(create_test_rgb_bitmap()))));
}

TEST_CASE(test_png)
{
    TRY_OR_FAIL((test_roundtrip<Gfx::PNGWriter, Gfx::PNGImageDecoderPlugin>(TRY_OR_FAIL(create_test_rgb_bitmap()))));
    TRY_OR_FAIL((test_roundtrip<Gfx::PNGWriter, Gfx::PNGImageDecoderPlugin>(TRY_OR_FAIL(create_test_rgba_bitmap()))));
}

TEST_CASE(test_webp)
{
    TRY_OR_FAIL((test_roundtrip<Gfx::WebPWriter, Gfx::WebPImageDecoderPlugin>(TRY_OR_FAIL(create_test_rgb_bitmap()))));
    TRY_OR_FAIL((test_roundtrip<Gfx::WebPWriter, Gfx::WebPImageDecoderPlugin>(TRY_OR_FAIL(create_test_rgba_bitmap()))));
}

TEST_CASE(test_webp_color_indexing_transform)
{
    Array<Color, 256> colors;
    for (size_t i = 0; i < colors.size(); ++i) {
        colors[i].set_red(i);
        colors[i].set_green(255 - i);
        colors[i].set_blue(128);
        colors[i].set_alpha(255 - i / 16);
    }
    for (int bits_per_pixel : { 1, 2, 4, 8 }) {
        int number_of_colors = 1 << bits_per_pixel;

        auto bitmap = TRY_OR_FAIL(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 47, 33 }));
        for (int y = 0; y < bitmap->height(); ++y)
            for (int x = 0; x < bitmap->width(); ++x)
                bitmap->set_pixel(x, y, colors[(x * bitmap->width() + y) % number_of_colors]);

        auto encoded_data = TRY_OR_FAIL(encode_bitmap<Gfx::WebPWriter>(bitmap));
        auto encoded_data_size = encoded_data.size();
        auto encoded_data_stream = TRY_OR_FAIL(adopt_nonnull_ref_or_enomem(new Gfx::ImageDecoderStream()));
        encoded_data_stream->append_chunk(move(encoded_data));
        encoded_data_stream->close();
        auto decoded_bitmap = TRY_OR_FAIL(expect_single_frame_of_size(*TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(move(encoded_data_stream))), bitmap->size()));
        expect_bitmaps_equal(*decoded_bitmap, *bitmap);

        Gfx::WebPEncoderOptions options;
        options.vp8l_options.allowed_transforms = 0;
        auto encoded_data_without_color_indexing = TRY_OR_FAIL(encode_bitmap<Gfx::WebPWriter>(bitmap, options));
        auto encoded_data_without_color_indexing_size = encoded_data_without_color_indexing.size();
        auto encoded_data_without_color_indexing_stream = TRY_OR_FAIL(adopt_nonnull_ref_or_enomem(new Gfx::ImageDecoderStream()));
        encoded_data_without_color_indexing_stream->append_chunk(move(encoded_data_without_color_indexing));
        encoded_data_without_color_indexing_stream->close();
        EXPECT(encoded_data_size < encoded_data_without_color_indexing_size);
        auto decoded_bitmap_without_color_indexing = TRY_OR_FAIL(expect_single_frame_of_size(*TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(move(encoded_data_without_color_indexing_stream))), bitmap->size()));
        expect_bitmaps_equal(*decoded_bitmap_without_color_indexing, *decoded_bitmap);
    }
}

TEST_CASE(test_webp_color_indexing_transform_single_channel)
{
    Array<Color, 256> colors;
    for (size_t i = 0; i < colors.size(); ++i) {
        colors[i].set_red(0);
        colors[i].set_green(255 - i);
        colors[i].set_blue(128);
        colors[i].set_alpha(255);
    }
    for (int bits_per_pixel : { 1, 2, 4, 8 }) {
        int number_of_colors = 1 << bits_per_pixel;

        auto bitmap = TRY_OR_FAIL(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 47, 33 }));
        for (int y = 0; y < bitmap->height(); ++y)
            for (int x = 0; x < bitmap->width(); ++x)
                bitmap->set_pixel(x, y, colors[(x * bitmap->width() + y) % number_of_colors]);

        auto encoded_data = TRY_OR_FAIL(encode_bitmap<Gfx::WebPWriter>(bitmap));
        auto encoded_data_size = encoded_data.size();
        auto encoded_data_stream = TRY_OR_FAIL(adopt_nonnull_ref_or_enomem(new Gfx::ImageDecoderStream()));
        encoded_data_stream->append_chunk(move(encoded_data));
        encoded_data_stream->close();
        auto decoded_bitmap = TRY_OR_FAIL(expect_single_frame_of_size(*TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(move(encoded_data_stream))), bitmap->size()));
        expect_bitmaps_equal(*decoded_bitmap, *bitmap);

        Gfx::WebPEncoderOptions options;
        options.vp8l_options.allowed_transforms = 0;
        auto encoded_data_without_color_indexing = TRY_OR_FAIL(encode_bitmap<Gfx::WebPWriter>(bitmap, options));
        auto encoded_data_without_color_indexing_size = encoded_data_without_color_indexing.size();
        auto encoded_data_without_color_indexing_stream = TRY_OR_FAIL(adopt_nonnull_ref_or_enomem(new Gfx::ImageDecoderStream()));
        encoded_data_without_color_indexing_stream->append_chunk(move(encoded_data_without_color_indexing));
        encoded_data_without_color_indexing_stream->close();
        if (bits_per_pixel == 8)
            EXPECT(encoded_data_size <= encoded_data_without_color_indexing_size);
        else
            EXPECT(encoded_data_size < encoded_data_without_color_indexing_size);
        auto decoded_bitmap_without_color_indexing = TRY_OR_FAIL(expect_single_frame_of_size(*TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(move(encoded_data_without_color_indexing_stream))), bitmap->size()));
        expect_bitmaps_equal(*decoded_bitmap_without_color_indexing, *decoded_bitmap);
    }
}
