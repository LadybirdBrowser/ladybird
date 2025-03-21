/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021, Brian Gianforcaro <bgianf@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/ImageFormats/AVIFLoader.h>
#include <LibGfx/ImageFormats/BMPLoader.h>
#include <LibGfx/ImageFormats/GIFLoader.h>
#include <LibGfx/ImageFormats/ICOLoader.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <LibGfx/ImageFormats/JPEGXLLoader.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/TIFFLoader.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>
#include <LibGfx/ImageFormats/TinyVGLoader.h>
#include <LibGfx/ImageFormats/WebPLoader.h>
#include <LibTest/TestCase.h>
#include <stdio.h>
#include <string.h>

#define TEST_INPUT(x) ("test-inputs/" x)

static ErrorOr<Gfx::ImageFrameDescriptor> expect_single_frame(Gfx::ImageDecoderPlugin& plugin_decoder)
{
    EXPECT_EQ(plugin_decoder.frame_count(), 1u);
    EXPECT(!plugin_decoder.is_animated());
    EXPECT(!plugin_decoder.loop_count());

    auto frame = TRY(plugin_decoder.frame(0));
    EXPECT_EQ(frame.duration, 0);
    return frame;
}

static ErrorOr<Gfx::ImageFrameDescriptor> expect_single_frame_of_size(Gfx::ImageDecoderPlugin& plugin_decoder, Gfx::IntSize size)
{
    EXPECT_EQ(plugin_decoder.size(), size);
    auto frame = TRY(expect_single_frame(plugin_decoder));
    EXPECT_EQ(frame.image->size(), size);
    return frame;
}

TEST_CASE(test_bmp)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("bmp/rgba32-1.bmp"sv)));
    EXPECT(Gfx::BMPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame(*plugin_decoder));
}

TEST_CASE(test_bmp_top_down)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("bmp/top-down.bmp"sv)));
    EXPECT(Gfx::BMPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame(*plugin_decoder));
    EXPECT_EQ(frame.image->format(), Gfx::BitmapFormat::RGBx8888);
    // Compares only rgb data
    EXPECT_EQ(frame.image->begin()[0] & 0x00ffffffU, 0x00dcc1b8U);
}

TEST_CASE(test_bmp_1bpp)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("bmp/bitmap.bmp"sv)));
    EXPECT(Gfx::BMPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 399, 400 }));
    EXPECT_EQ(frame.image->begin()[0], 0xff'ff'ff'ff);
}

TEST_CASE(test_ico_malformed_frame)
{
    Array test_inputs = {
        TEST_INPUT("ico/oss-fuzz-testcase-62541.ico"sv),
        TEST_INPUT("ico/oss-fuzz-testcase-63177.ico"sv),
        TEST_INPUT("ico/oss-fuzz-testcase-63357.ico"sv)
    };

    for (auto test_input : test_inputs) {
        auto file = TRY_OR_FAIL(Core::MappedFile::map(test_input));
        auto plugin_decoder = TRY_OR_FAIL(Gfx::ICOImageDecoderPlugin::create(file->bytes()));
        auto frame_or_error = plugin_decoder->frame(0);
        EXPECT(frame_or_error.is_error());
    }
}

TEST_CASE(test_gif)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("gif/download-animation.gif"sv)));
    EXPECT(Gfx::GIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::GIFImageDecoderPlugin::create(file->bytes()));

    EXPECT(plugin_decoder->frame_count());
    EXPECT(plugin_decoder->is_animated());
    EXPECT(!plugin_decoder->loop_count());

    auto frame = TRY_OR_FAIL(plugin_decoder->frame(1));
    EXPECT(frame.duration == 400);
}

TEST_CASE(test_corrupted_gif)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("gif/corrupted.gif"sv)));
    EXPECT(Gfx::GIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::GIFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(plugin_decoder->frame(0));
    EXPECT_EQ(plugin_decoder->frame_count(), 1u);
}

TEST_CASE(test_gif_without_global_color_table)
{
    Array<u8, 35> gif_data {
        // Header (6 bytes): "GIF89a"
        0x47,
        0x49,
        0x46,
        0x38,
        0x39,
        0x61,

        // Logical Screen Descriptor (7 bytes)
        0x01,
        0x00, // Width (1)
        0x01,
        0x00, // Height (1)
        0x00, // Packed fields (NOTE: the MSB here is the Global Color Table flag!)
        0x00, // Background Color Index
        0x00, // Pixel Aspect Ratio

        // Image Descriptor (10 bytes)
        0x2C,
        0x00,
        0x00,
        0x00,
        0x00,
        0x01,
        0x00,
        0x01,
        0x00,
        0x80,

        // Local Color Table (6 bytes: 2 colors, 3 bytes per color)
        0x00,
        0x00,
        0x00, // Color 1: Black (RGB: 0, 0, 0)
        0xff,
        0x00,
        0x00, // Color 2: Red (RGB: 255, 0, 0)

        // Image Data (8 bytes)
        0x02, // LZW Minimum Code Size
        0x02, // Data Sub-block size (2 bytes)
        0x4C,
        0x01, // Image Data
        0x00, // Data Sub-block Terminator

        // Trailer (1 byte)
        0x3B,
    };

    auto plugin_decoder = TRY_OR_FAIL(Gfx::GIFImageDecoderPlugin::create(gif_data));
    EXPECT_EQ(plugin_decoder->frame_count(), 1u);
    auto frame = TRY_OR_FAIL(plugin_decoder->frame(0));
    EXPECT(frame.image);
    EXPECT_EQ(frame.image->size(), Gfx::IntSize(1, 1));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_not_ico)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("png/buggie.png"sv)));
    EXPECT(!Gfx::ICOImageDecoderPlugin::sniff(file->bytes()));
    EXPECT(Gfx::ICOImageDecoderPlugin::create(file->bytes()).is_error());
}

TEST_CASE(test_bmp_embedded_in_ico)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("ico/serenity.ico"sv)));
    EXPECT(Gfx::ICOImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::ICOImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 16, 16 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Transparent);
    EXPECT_EQ(frame.image->get_pixel(7, 4), Gfx::Color(161, 0, 0));
}

TEST_CASE(test_malformed_maskless_ico)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("ico/malformed_maskless.ico"sv)));
    EXPECT(Gfx::ICOImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::ICOImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 16, 16 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Transparent);
    EXPECT_EQ(frame.image->get_pixel(7, 4), Gfx::Color(161, 0, 0));
}

TEST_CASE(test_jpeg_sof0_one_scan)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/rgb24.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame(*plugin_decoder));
}

TEST_CASE(test_jpeg_sof0_several_scans)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/several_scans.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 592, 800 }));
}

TEST_CASE(test_odd_mcu_restart_interval)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/odd-restart.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 102, 77 }));
}

TEST_CASE(test_jpeg_rgb_components)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/rgb_components.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 592, 800 }));
}

TEST_CASE(test_jpeg_ycck)
{
    Array test_inputs = {
        TEST_INPUT("jpg/ycck-1111.jpg"sv),
        TEST_INPUT("jpg/ycck-2111.jpg"sv),
        TEST_INPUT("jpg/ycck-2112.jpg"sv),
    };

    for (auto test_input : test_inputs) {
        auto file = TRY_OR_FAIL(Core::MappedFile::map(test_input));
        EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
        auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));
        auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 592, 800 }));

        // Compare difference between pixels so we don't depend on exact CMYK->RGB conversion behavior.
        // These two pixels are currently off by one in R.
        // FIXME: For 2111, they're off by way more.
        EXPECT(frame.image->get_pixel(6, 319).distance_squared_to(frame.image->get_pixel(6, 320)) < 1.0f / 255.0f);
    }
}

TEST_CASE(test_jpeg_sof2_spectral_selection)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/spectral_selection.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 592, 800 }));
}

TEST_CASE(test_jpeg_sof0_several_scans_odd_number_mcu)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/several_scans_odd_number_mcu.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 600, 600 }));
}

TEST_CASE(test_jpeg_sof2_successive_aproximation)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/successive_approximation.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 600, 800 }));
}

TEST_CASE(test_jpeg_empty_icc)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/gradient_empty_icc.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 80, 80 }));
}

TEST_CASE(test_jpeg_grayscale_with_app14)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/grayscale_app14.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 80, 80 }));
}

TEST_CASE(test_jpeg_grayscale_with_weird_mcu_and_reset_marker)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/grayscale_mcu.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 320, 240 }));
}

TEST_CASE(test_jpeg_malformed_header)
{
    Array test_inputs = {
        TEST_INPUT("jpg/oss-fuzz-testcase-59785.jpg"sv)
    };

    for (auto test_input : test_inputs) {
        auto file = TRY_OR_FAIL(Core::MappedFile::map(test_input));
        auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));
        auto frame_or_error = plugin_decoder->frame(0);
        EXPECT(frame_or_error.is_error());
    }
}

TEST_CASE(test_jpeg_malformed_frame)
{
    Array test_inputs = {
        TEST_INPUT("jpg/oss-fuzz-testcase-62584.jpg"sv),
        TEST_INPUT("jpg/oss-fuzz-testcase-63815.jpg"sv)
    };

    for (auto test_input : test_inputs) {
        auto file = TRY_OR_FAIL(Core::MappedFile::map(test_input));
        auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));
        auto frame_or_error = plugin_decoder->frame(0);
        EXPECT(frame_or_error.is_error());
    }
}

TEST_CASE(test_png)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("png/buggie.png"sv)));
    EXPECT(Gfx::PNGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::PNGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame(*plugin_decoder));
}

TEST_CASE(test_exif)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("png/exif.png"sv)));
    EXPECT(Gfx::PNGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::PNGImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 200, 100 }));
    EXPECT(plugin_decoder->metadata().has_value());
    auto const& exif_metadata = static_cast<Gfx::ExifMetadata const&>(plugin_decoder->metadata().value());
    EXPECT_EQ(*exif_metadata.orientation(), Gfx::TIFF::Orientation::Rotate90Clockwise);

    EXPECT_EQ(frame.image->get_pixel(65, 70), Gfx::Color(0, 255, 0));
    EXPECT_EQ(frame.image->get_pixel(190, 10), Gfx::Color(255, 0, 0));
}

TEST_CASE(test_png_malformed_frame)
{
    Array test_inputs = {
        TEST_INPUT("png/oss-fuzz-testcase-62371.png"sv),
        TEST_INPUT("png/oss-fuzz-testcase-63052.png"sv)
    };

    for (auto test_input : test_inputs) {
        auto file = TRY_OR_FAIL(Core::MappedFile::map(test_input));
        auto plugin_decoder_or_error = Gfx::PNGImageDecoderPlugin::create(file->bytes());
        if (plugin_decoder_or_error.is_error())
            continue;
        auto plugin_decoder = plugin_decoder_or_error.release_value();
        auto frame_or_error = plugin_decoder->frame(0);
        EXPECT(frame_or_error.is_error());
    }
}

TEST_CASE(test_tiff_uncompressed)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/uncompressed.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_ccitt_rle)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/ccitt_rle.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Black);
}

TEST_CASE(test_tiff_ccitt3)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/ccitt3.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Black);
}

TEST_CASE(test_tiff_ccitt3_no_tags)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/ccitt3_no_tags.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 6, 4 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(3, 0), Gfx::Color::NamedColor::Black);
    EXPECT_EQ(frame.image->get_pixel(2, 2), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(5, 3), Gfx::Color::NamedColor::White);
}

TEST_CASE(test_tiff_ccitt3_fill)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/ccitt3_1d_fill.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 6, 4 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(3, 0), Gfx::Color::NamedColor::Black);
    EXPECT_EQ(frame.image->get_pixel(2, 2), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(5, 3), Gfx::Color::NamedColor::White);
}

TEST_CASE(test_tiff_ccitt3_2d)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/ccitt3_2d.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Black);
}

TEST_CASE(test_tiff_ccitt3_2d_fill)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/ccitt3_2d_fill.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Black);
}

TEST_CASE(test_tiff_ccitt4)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/ccitt4.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Black);
}

TEST_CASE(test_tiff_lzw)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/lzw.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_deflate)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/deflate.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_krita)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/krita.tif"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_orientation)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/orientation.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 300, 400 }));

    // Orientation is Rotate90Clockwise
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(300 - 75, 60), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_packed_bits)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/packed_bits.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_grayscale)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/grayscale.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color(130, 130, 130));
}

TEST_CASE(test_tiff_grayscale_alpha)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/grayscale_alpha.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0).alpha(), 0);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color(130, 130, 130));
}

TEST_CASE(test_tiff_rgb_alpha)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/rgb_alpha.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0).alpha(), 0);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_palette_alpha)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/rgb_palette_alpha.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0).alpha(), 0);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_alpha_predictor)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/alpha_predictor.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0).alpha(), 255);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_16_bits)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/16_bits.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_cmyk)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/cmyk.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    // I stripped the ICC profile from the image, so we can't test for equality with Red here.
    EXPECT_NE(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::White);
}

TEST_CASE(test_tiff_tiled)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/tiled.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 300 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::White);
    EXPECT_EQ(frame.image->get_pixel(60, 75), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_tiff_invalid_tag)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tiff/invalid_tag.tiff"sv)));
    EXPECT(Gfx::TIFFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 10, 10 }));

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Black);
    EXPECT_EQ(frame.image->get_pixel(0, 9), Gfx::Color::NamedColor::White);
}

TEST_CASE(test_webp_simple_lossy)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/simple-vp8.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 240, 240 }));

    // While VP8 YUV contents are defined bit-exact, the YUV->RGB conversion isn't.
    // So pixels changing by 1 or so below is fine if you change code.
    EXPECT_EQ(frame.image->get_pixel(120, 232), Gfx::Color(0xf1, 0xef, 0xf0, 255));
    EXPECT_EQ(frame.image->get_pixel(198, 202), Gfx::Color(0x7a, 0xaa, 0xd5, 255));
}

TEST_CASE(test_webp_simple_lossless)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/simple-vp8l.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    // Ironically, simple-vp8l.webp is a much more complex file than extended-lossless.webp tested below.
    // extended-lossless.webp tests the decoding basics.
    // This here tests the predictor, color, and subtract green transforms,
    // as well as meta prefix images, one-element canonical code handling,
    // and handling of canonical codes with more than 288 elements.
    // This image uses all 13 predictor modes of the predictor transform.
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 386, 395 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0, 0, 0, 0));

    // This pixel tests all predictor modes except 5, 7, 8, 9, and 13.
    EXPECT_EQ(frame.image->get_pixel(289, 332), Gfx::Color(0xf2, 0xee, 0xd3, 255));
}

TEST_CASE(test_webp_simple_lossless_alpha_used_false)
{
    // This file is identical to simple-vp8l.webp, but the `is_alpha_used` used bit is false.
    // The file still contains alpha data. This tests that the decoder replaces the stored alpha data with 0xff if `is_alpha_used` is false.
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/simple-vp8l-alpha-used-false.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 386, 395 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0, 0, 0, 0xff));
}

TEST_CASE(test_webp_extended_lossy)
{
    // This extended lossy image has an ALPH chunk for (losslessly compressed) alpha data.
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/extended-lossy.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 417, 223 }));

    // While VP8 YUV contents are defined bit-exact, the YUV->RGB conversion isn't.
    // So pixels changing by 1 or so below is fine if you change code.
    EXPECT_EQ(frame.image->get_pixel(89, 72), Gfx::Color(255, 1, 0, 255));
    EXPECT_EQ(frame.image->get_pixel(174, 69), Gfx::Color(0, 255, 0, 255));
    EXPECT_EQ(frame.image->get_pixel(245, 84), Gfx::Color(0, 0, 255, 255));
    EXPECT_EQ(frame.image->get_pixel(352, 125), Gfx::Color(0, 0, 0, 128));
    EXPECT_EQ(frame.image->get_pixel(355, 106), Gfx::Color(0, 0, 0, 0));

    // Check same basic pixels as in test_webp_extended_lossless too.
    // (The top-left pixel in the lossy version is fully transparent white, compared to fully transparent black in the lossless version).
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(255, 255, 255, 0));
    EXPECT_EQ(frame.image->get_pixel(43, 75), Gfx::Color(255, 0, 2, 255));
    EXPECT_EQ(frame.image->get_pixel(141, 75), Gfx::Color(0, 255, 3, 255));
    EXPECT_EQ(frame.image->get_pixel(235, 75), Gfx::Color(0, 0, 255, 255));
    EXPECT_EQ(frame.image->get_pixel(341, 75), Gfx::Color(0, 0, 0, 128));
}

TEST_CASE(test_webp_extended_lossy_alpha_horizontal_filter)
{
    // Also lossy rgb + lossless alpha, but with a horizontal alpha filtering method.
    // The image should look like smolkling.webp, but with a horizontal alpha gradient.
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/smolkling-horizontal-alpha.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 264, 264 }));

    // While VP8 YUV contents are defined bit-exact, the YUV->RGB conversion isn't.
    // So pixels changing by 1 or so below is fine if you change code.
    // The important component in this test is alpha, and that shouldn't change even by 1 as it's losslessly compressed and doesn't use YUV.
    EXPECT_EQ(frame.image->get_pixel(131, 131), Gfx::Color(0x8f, 0x50, 0x33, 0x4b));
}

TEST_CASE(test_webp_extended_lossy_alpha_vertical_filter)
{
    // Also lossy rgb + lossless alpha, but with a vertical alpha filtering method.
    // The image should look like smolkling.webp, but with a vertical alpha gradient, and with a fully transparent first column.
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/smolkling-vertical-alpha.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 264, 264 }));

    // While VP8 YUV contents are defined bit-exact, the YUV->RGB conversion isn't.
    // So pixels changing by 1 or so below is fine if you change code.
    // The important component in this test is alpha, and that shouldn't change even by 1 as it's losslessly compressed and doesn't use YUV.
    EXPECT_EQ(frame.image->get_pixel(131, 131), Gfx::Color(0x92, 0x50, 0x32, 0x4c));
}

TEST_CASE(test_webp_extended_lossy_alpha_gradient_filter)
{
    // Also lossy rgb + lossless alpha, but with a gradient alpha filtering method.
    // The image should look like smolkling.webp, but with a few transparent pixels in the shape of a C on it. Most of the image should not be transparent.
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/smolkling-gradient-alpha.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 264, 264 }));

    // While VP8 YUV contents are defined bit-exact, the YUV->RGB conversion isn't.
    // So pixels changing by 1 or so below is fine if you change code.
    // The important component in this test is alpha, and that shouldn't change even by 1 as it's losslessly compressed and doesn't use YUV.
    // In particular, the center of the image should be fully opaque, not fully transparent.
    EXPECT_EQ(frame.image->get_pixel(131, 131), Gfx::Color(0x8a, 0x48, 0x2e, 255));
}

TEST_CASE(test_webp_extended_lossy_uncompressed_alpha)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/extended-lossy-uncompressed-alpha.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 417, 223 }));

    // While VP8 YUV contents are defined bit-exact, the YUV->RGB conversion isn't.
    // So pixels changing by 1 or so below is fine if you change code.
    EXPECT_EQ(frame.image->get_pixel(89, 72), Gfx::Color(254, 0, 6, 255));
    EXPECT_EQ(frame.image->get_pixel(174, 69), Gfx::Color(0, 255, 0, 255));
    EXPECT_EQ(frame.image->get_pixel(245, 84), Gfx::Color(0, 0, 255, 255));
    EXPECT_EQ(frame.image->get_pixel(352, 125), Gfx::Color(0, 0, 0, 128));
    EXPECT_EQ(frame.image->get_pixel(355, 106), Gfx::Color(0, 0, 0, 0));
}

TEST_CASE(test_webp_extended_lossy_negative_quantization_offset)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/smolkling.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 264, 264 }));

    // While VP8 YUV contents are defined bit-exact, the YUV->RGB conversion isn't.
    // So pixels changing by 1 or so below is fine if you change code.
    EXPECT_EQ(frame.image->get_pixel(16, 16), Gfx::Color(0x3b, 0x25, 0x18, 255));
}

TEST_CASE(test_webp_lossy_4)
{
    // This is https://commons.wikimedia.org/wiki/File:Fr%C3%BChling_bl%C3%BChender_Kirschenbaum.jpg,
    // under the Creative Commons Attribution-Share Alike 3.0 Unported license. The image was re-encoded
    // as webp at https://developers.google.com/speed/webp/gallery1 and the webp version is from there.
    // No other changes have been made.
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/4.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1024, 772 }));

    // This image tests macroblocks that have `skip_coefficients` set to true, and it test a boolean entropy decoder edge case.
    EXPECT_EQ(frame.image->get_pixel(780, 570), Gfx::Color(0x72, 0xc8, 0xf6, 255));
}

TEST_CASE(test_webp_lossy_4_with_partitions)
{
    // Same input file as in the previous test, but re-encoded to use 8 secondary partitions.
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/4-with-8-partitions.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1024, 772 }));
    EXPECT_EQ(frame.image->get_pixel(780, 570), Gfx::Color(0x72, 0xc7, 0xf8, 255));
}

TEST_CASE(test_webp_extended_lossless)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/extended-lossless.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 417, 223 }));

    // Check some basic pixels.
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0, 0, 0, 0));
    EXPECT_EQ(frame.image->get_pixel(43, 75), Gfx::Color(255, 0, 0, 255));
    EXPECT_EQ(frame.image->get_pixel(141, 75), Gfx::Color(0, 255, 0, 255));
    EXPECT_EQ(frame.image->get_pixel(235, 75), Gfx::Color(0, 0, 255, 255));
    EXPECT_EQ(frame.image->get_pixel(341, 75), Gfx::Color(0, 0, 0, 128));

    // Check pixels using the color cache.
    EXPECT_EQ(frame.image->get_pixel(94, 73), Gfx::Color(255, 0, 0, 255));
    EXPECT_EQ(frame.image->get_pixel(176, 115), Gfx::Color(0, 255, 0, 255));
    EXPECT_EQ(frame.image->get_pixel(290, 89), Gfx::Color(0, 0, 255, 255));
    EXPECT_EQ(frame.image->get_pixel(359, 73), Gfx::Color(0, 0, 0, 128));
}

TEST_CASE(test_webp_simple_lossless_color_index_transform)
{
    // In addition to testing the index transform, this file also tests handling of explicity setting max_symbol.
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/Qpalette.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 256, 256 }));

    EXPECT_EQ(frame.image->get_pixel(100, 100), Gfx::Color(0x73, 0x37, 0x23, 0xff));
}

TEST_CASE(test_webp_simple_lossless_color_index_transform_pixel_bundling)
{
    struct TestCase {
        StringView file_name;
        Gfx::Color line_color;
        Gfx::Color background_color;
    };

    // The number after the dash is the number of colors in each file's color index bitmap.
    // catdog-alert-2 tests the 1-bit-per-pixel case,
    // catdog-alert-3 tests the 2-bit-per-pixel case,
    // catdog-alert-8 and catdog-alert-13 both test the 4-bits-per-pixel case.
    // catdog-alert-13-alpha-used-false is like catdog-alert-13, but with is_alpha_used set to false in the header
    // (which has the effect of ignoring the alpha information in the palette and instead always setting alpha to 0xff).
    TestCase test_cases[] = {
        { "webp/catdog-alert-2.webp"sv, Gfx::Color(0x35, 0x12, 0x0a, 0xff), Gfx::Color(0xf3, 0xe6, 0xd8, 0xff) },
        { "webp/catdog-alert-3.webp"sv, Gfx::Color(0x35, 0x12, 0x0a, 0xff), Gfx::Color(0, 0, 0, 0) },
        { "webp/catdog-alert-8.webp"sv, Gfx::Color(0, 0, 0, 255), Gfx::Color(0, 0, 0, 0) },
        { "webp/catdog-alert-13.webp"sv, Gfx::Color(0, 0, 0, 255), Gfx::Color(0, 0, 0, 0) },
        { "webp/catdog-alert-13-alpha-used-false.webp"sv, Gfx::Color(0, 0, 0, 255), Gfx::Color(0, 0, 0, 255) },
    };

    for (auto test_case : test_cases) {
        auto file = TRY_OR_FAIL(Core::MappedFile::map(MUST(String::formatted("{}{}", TEST_INPUT(""), test_case.file_name))));
        EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
        auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

        auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 32, 32 }));

        EXPECT_EQ(frame.image->get_pixel(4, 0), test_case.background_color);
        EXPECT_EQ(frame.image->get_pixel(5, 0), test_case.line_color);

        EXPECT_EQ(frame.image->get_pixel(9, 5), test_case.background_color);
        EXPECT_EQ(frame.image->get_pixel(10, 5), test_case.line_color);
        EXPECT_EQ(frame.image->get_pixel(11, 5), test_case.background_color);
    }
}

TEST_CASE(test_webp_simple_lossless_color_index_transform_pixel_bundling_odd_width)
{
    StringView file_names[] = {
        "webp/width11-height11-colors2.webp"sv,
        "webp/width11-height11-colors3.webp"sv,
        "webp/width11-height11-colors15.webp"sv,
    };

    for (auto file_name : file_names) {
        auto file = TRY_OR_FAIL(Core::MappedFile::map(MUST(String::formatted("{}{}", TEST_INPUT(""), file_name))));
        auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));
        TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 11, 11 }));
    }
}

TEST_CASE(test_webp_extended_lossless_animated)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/extended-lossless-animated.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    EXPECT_EQ(plugin_decoder->loop_count(), 42u);
    EXPECT_EQ(plugin_decoder->frame_count(), 8u);
    EXPECT(plugin_decoder->is_animated());

    EXPECT_EQ(plugin_decoder->size(), Gfx::IntSize(990, 1050));

    for (size_t frame_index = 0; frame_index < plugin_decoder->frame_count(); ++frame_index) {
        auto frame = TRY_OR_FAIL(plugin_decoder->frame(frame_index));
        EXPECT_EQ(frame.image->size(), Gfx::IntSize(990, 1050));

        // This pixel happens to be the same color in all frames.
        EXPECT_EQ(frame.image->get_pixel(500, 700), Gfx::Color::Yellow);

        // This one isn't the same in all frames.
        EXPECT_EQ(frame.image->get_pixel(500, 0), (frame_index == 2 || frame_index == 6) ? Gfx::Color::Black : Gfx::Color(0, 0, 0, 0));
    }
}

TEST_CASE(test_webp_unpremultiplied_alpha)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/semi-transparent-pixel.webp"sv)));
    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1, 1 }));

    // Webp decodes with unpremultiplied color data, so {R,G,B} can be >A (unlike with premultiplied colors).
    EXPECT_EQ(frame.image->alpha_type(), Gfx::AlphaType::Unpremultiplied);
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(255, 255, 255, 128));
}

TEST_CASE(test_tvg)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tvg/yak.tvg"sv)));
    EXPECT(Gfx::TinyVGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TinyVGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1024, 1024 }));
}

TEST_CASE(test_everything_tvg)
{
    Array file_names {
        TEST_INPUT("tvg/everything.tvg"sv),
        TEST_INPUT("tvg/everything-32.tvg"sv)
    };

    for (auto file_name : file_names) {
        auto file = TRY_OR_FAIL(Core::MappedFile::map(file_name));
        EXPECT(Gfx::TinyVGImageDecoderPlugin::sniff(file->bytes()));
        auto plugin_decoder = TRY_OR_FAIL(Gfx::TinyVGImageDecoderPlugin::create(file->bytes()));

        TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 400, 768 }));
    }
}

TEST_CASE(test_tvg_malformed)
{
    Array test_inputs = {
        TEST_INPUT("tvg/bogus-color-table-size.tvg"sv)
    };

    for (auto test_input : test_inputs) {
        auto file = TRY_OR_FAIL(Core::MappedFile::map(test_input));
        auto plugin_decoder = TRY_OR_FAIL(Gfx::TinyVGImageDecoderPlugin::create(file->bytes()));
        auto frame_or_error = plugin_decoder->frame(0);
        EXPECT(frame_or_error.is_error());
    }
}

TEST_CASE(test_tvg_rgb565)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("tvg/green-rgb565.tvg"sv)));
    EXPECT(Gfx::TinyVGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::TinyVGImageDecoderPlugin::create(file->bytes()));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 100, 100 }));

    // Should be a solid dark green:
    EXPECT_EQ(frame.image->get_pixel(50, 50), Gfx::Color(0, 130, 0));
}

TEST_CASE(test_jxl_modular_simple_tree_upsample2_10bits)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jxl/modular_simple_tree_upsample2_10bits_rct.jxl"sv)));
    EXPECT(Gfx::JPEGXLImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGXLImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 128, 128 }));

    auto frame = TRY_OR_FAIL(plugin_decoder->frame(0));
}

TEST_CASE(test_avif_simple_lossy)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("avif/simple-lossy.avif"sv)));
    EXPECT(Gfx::AVIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::AVIFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 240, 240 }));

    // While AVIF YUV contents are defined bit-exact, the YUV->RGB conversion isn't.
    // So pixels changing by 1 or so below is fine if you change code.
    EXPECT_EQ(frame.image->get_pixel(120, 232), Gfx::Color(0xf1, 0xef, 0xf0, 255));
    EXPECT_EQ(frame.image->get_pixel(198, 202), Gfx::Color(0x7b, 0xaa, 0xd6, 255));
}

TEST_CASE(test_avif_simple_lossless)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("avif/simple-lossless.avif"sv)));
    EXPECT(Gfx::AVIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::AVIFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 386, 395 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0, 0, 0, 0));
    EXPECT_EQ(frame.image->get_pixel(289, 332), Gfx::Color(0xf2, 0xee, 0xd3, 255));
}

TEST_CASE(test_avif_simple_lossy_bitdepth10)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("avif/simple-bitdepth10.avif"sv)));
    EXPECT(!Gfx::AVIFImageDecoderPlugin::sniff(file->bytes()));
}

TEST_CASE(test_avif_icc_profile)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("avif/icc_profile.avif"sv)));
    EXPECT(Gfx::AVIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::AVIFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 240, 240 }));
    EXPECT(TRY_OR_FAIL(plugin_decoder->icc_data()).has_value());
}

TEST_CASE(test_avif_no_icc_profile)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("avif/simple-lossy.avif"sv)));
    EXPECT(Gfx::AVIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::AVIFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 240, 240 }));
    EXPECT(!TRY_OR_FAIL(plugin_decoder->icc_data()).has_value());
}

TEST_CASE(test_avif_frame_out_of_bounds)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("avif/simple-lossy.avif"sv)));
    EXPECT(Gfx::AVIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::AVIFImageDecoderPlugin::create(file->bytes()));

    auto frame1 = TRY_OR_FAIL(plugin_decoder->frame(0));
    EXPECT(plugin_decoder->frame(1).is_error());
}

TEST_CASE(test_avif_missing_pixi_property)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("avif/missing-pixi-property.avif"sv)));
    EXPECT(Gfx::AVIFImageDecoderPlugin::sniff(file->bytes()));
}
