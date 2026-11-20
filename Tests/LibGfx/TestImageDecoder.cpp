/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021, Brian Gianforcaro <bgianf@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/ImageFormats/AVIFLoader.h>
#include <LibGfx/ImageFormats/BMPLoader.h>
#include <LibGfx/ImageFormats/ExifReader.h>
#include <LibGfx/ImageFormats/GIFLoader.h>
#include <LibGfx/ImageFormats/ICOLoader.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <LibGfx/ImageFormats/JPEGXLLoader.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>
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

static void append_u16(Vector<u8>& data, u16 value)
{
    data.append(value & 0xff);
    data.append(value >> 8);
}

static void append_u32(Vector<u8>& data, u32 value)
{
    append_u16(data, value & 0xffff);
    append_u16(data, value >> 16);
}

static void append_bitmap_info_header(Vector<u8>& data, u32 header_size, i32 width, i32 height, u16 bits_per_pixel, u32 compression, u32 palette_colors)
{
    append_u32(data, header_size);
    append_u32(data, width);
    append_u32(data, height);
    append_u16(data, 1); // Plane count.
    append_u16(data, bits_per_pixel);
    append_u32(data, compression);
    append_u32(data, 0); // Pixel data size.
    append_u32(data, 0); // Horizontal resolution.
    append_u32(data, 0); // Vertical resolution.
    append_u32(data, palette_colors);
    append_u32(data, 0); // Important palette colors.
}

static Vector<u8> make_bmp_file(ReadonlyBytes dib, size_t pixel_offset_in_dib)
{
    Vector<u8> data;
    data.append('B');
    data.append('M');
    append_u32(data, 14 + dib.size()); // File size.
    append_u32(data, 0);               // Reserved.
    append_u32(data, 14 + pixel_offset_in_dib);
    data.append(dib.data(), dib.size());
    return data;
}

static Vector<u8> make_ico_file(u8 width, u8 height, u16 bits_per_pixel, ReadonlyBytes image)
{
    Vector<u8> data;
    append_u16(data, 0); // Reserved.
    append_u16(data, 1); // Icon type.
    append_u16(data, 1); // Image count.
    data.append(width);
    data.append(height);
    data.append(0);      // Palette colors.
    data.append(0);      // Reserved.
    append_u16(data, 1); // Plane count.
    append_u16(data, bits_per_pixel);
    append_u32(data, image.size());
    append_u32(data, 22); // Image offset.
    data.append(image.data(), image.size());
    return data;
}

static ErrorOr<ByteBuffer> make_webp_with_declared_but_missing_iccp_chunk(ReadonlyBytes simple_webp)
{
    VERIFY(simple_webp.size() >= 20);
    VERIFY(simple_webp.slice(0, 4) == "RIFF"sv.bytes());
    VERIFY(simple_webp.slice(8, 4) == "WEBP"sv.bytes());
    VERIFY(simple_webp.slice(12, 4) == "VP8 "sv.bytes());

    auto vp8_chunk = simple_webp.slice(12);

    ByteBuffer extended_webp;
    auto append_u32_le = [&](u32 value) -> ErrorOr<void> {
        TRY(extended_webp.try_append(static_cast<u8>(value)));
        TRY(extended_webp.try_append(static_cast<u8>(value >> 8)));
        TRY(extended_webp.try_append(static_cast<u8>(value >> 16)));
        TRY(extended_webp.try_append(static_cast<u8>(value >> 24)));
        return {};
    };
    auto append_u24_le = [&](u32 value) -> ErrorOr<void> {
        VERIFY(value <= 0xffffff);
        TRY(extended_webp.try_append(static_cast<u8>(value)));
        TRY(extended_webp.try_append(static_cast<u8>(value >> 8)));
        TRY(extended_webp.try_append(static_cast<u8>(value >> 16)));
        return {};
    };

    TRY(extended_webp.try_append("RIFF"sv.bytes()));
    TRY(append_u32_le(4 + 18 + vp8_chunk.size()));
    TRY(extended_webp.try_append("WEBP"sv.bytes()));

    TRY(extended_webp.try_append("VP8X"sv.bytes()));
    TRY(append_u32_le(10));
    TRY(extended_webp.try_append(0x20)); // ICCP flag, without a corresponding ICCP chunk.
    TRY(extended_webp.try_append(0));
    TRY(extended_webp.try_append(0));
    TRY(extended_webp.try_append(0));
    TRY(append_u24_le(239)); // simple-vp8.webp is 240x240, encoded as dimension - 1.
    TRY(append_u24_le(239));

    TRY(extended_webp.try_append(vp8_chunk));
    return extended_webp;
}

TEST_CASE(test_frames_are_decoded_into_shareable_bitmaps)
{
    Array paths {
        TEST_INPUT("avif/simple-lossy.avif"sv),
        TEST_INPUT("bmp/bitmap.bmp"sv),
        TEST_INPUT("gif/download-animation.gif"sv),
        TEST_INPUT("ico/serenity.ico"sv),
        TEST_INPUT("jpg/rgb24.jpg"sv),
        TEST_INPUT("jpg/cmyk-no-adobe-marker.jpg"sv),
        TEST_INPUT("jpg/ycck-1111.jpg"sv),
        TEST_INPUT("jxl/modular_simple_tree_upsample2_10bits_rct.jxl"sv),
        TEST_INPUT("png/buggie.png"sv),
        TEST_INPUT("png/apng-blend.png"sv),
        TEST_INPUT("png/corrupt-idat.png"sv),
        TEST_INPUT("png/exif-orientation-5.png"sv),
        TEST_INPUT("webp/simple-vp8.webp"sv),
        TEST_INPUT("webp/extended-lossless-animated.webp"sv),
    };
    for (auto path : paths) {
        auto file = TRY_OR_FAIL(Core::MappedFile::map(path));
        auto decoder = TRY_OR_FAIL(Gfx::ImageDecoder::try_create_for_raw_bytes(file->bytes()));
        EXPECT(decoder);
        if (!decoder)
            continue;
        for (size_t index = 0; index < decoder->frame_count(); ++index) {
            auto frame = TRY_OR_FAIL(decoder->frame(index));
            if (!frame.image->is_in_shareable_memory())
                FAIL(ByteString::formatted("Frame {} of {} is not shareable", index, path));
        }
    }
}

TEST_CASE(test_bmp)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("bmp/rgba32-1.bmp"sv)));
    EXPECT(Gfx::BMPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame(*plugin_decoder));
}

TEST_CASE(test_bmp_v2_rejects_rle8_with_16bpp)
{
    Array<u8, 70> bmp_data {};
    bmp_data[0] = 'B';
    bmp_data[1] = 'M';
    bmp_data[2] = 70;  // File size.
    bmp_data[10] = 66; // Pixel offset.
    bmp_data[14] = 52; // BITMAPV2INFOHEADER size.
    bmp_data[18] = 1;  // Width.
    bmp_data[22] = 1;  // Height.
    bmp_data[26] = 1;  // Plane count.
    bmp_data[28] = 16; // Bits per pixel.
    bmp_data[30] = 1;  // BI_RLE8 compression.
    bmp_data[34] = 4;  // Pixel data size.
    bmp_data[66] = 1;
    bmp_data[67] = 1;
    bmp_data[69] = 1;

    EXPECT(Gfx::BMPImageDecoderPlugin::create(bmp_data).is_error());
}

TEST_CASE(test_bmp_v3_rgb16_ignores_embedded_masks)
{
    Array<u8, 74> bmp_data {};
    bmp_data[0] = 'B';
    bmp_data[1] = 'M';
    bmp_data[2] = 74;    // File size.
    bmp_data[10] = 70;   // Pixel offset.
    bmp_data[14] = 56;   // BITMAPV3INFOHEADER size.
    bmp_data[18] = 1;    // Width.
    bmp_data[22] = 1;    // Height.
    bmp_data[26] = 1;    // Plane count.
    bmp_data[28] = 16;   // Bits per pixel.
    bmp_data[34] = 4;    // Pixel data size.
    bmp_data[71] = 0x7c; // Pixel data: RGB555 red.

    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(bmp_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_bmp_v3_rgb32_completes_alpha_masks)
{
    Array<u8, 74> bmp_data {};
    bmp_data[0] = 'B';
    bmp_data[1] = 'M';
    bmp_data[2] = 74;  // File size.
    bmp_data[10] = 70; // Pixel offset.
    bmp_data[14] = 56; // BITMAPV3INFOHEADER size.
    bmp_data[18] = 1;  // Width.
    bmp_data[22] = 1;  // Height.
    bmp_data[26] = 1;  // Plane count.
    bmp_data[28] = 32; // Bits per pixel.
    bmp_data[34] = 4;  // Pixel data size.
    bmp_data[69] = 0xff;
    bmp_data[70] = 0x33;
    bmp_data[71] = 0x22;
    bmp_data[72] = 0x11;
    bmp_data[73] = 0xff;

    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(bmp_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0x11, 0x22, 0x33));
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

TEST_CASE(test_bmp_too_many_palette_colors)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("bmp/too-many-palette-colors.bmp"sv)));
    EXPECT(Gfx::BMPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 2, 2 }));
}

TEST_CASE(test_bmp_v4)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("bmp/2x2x32_v4.bmp"sv)));
    EXPECT(Gfx::BMPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 2, 2 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_bmp_negative_int_min_height)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("bmp/negative-height-int-min.bmp"sv)));
    EXPECT(Gfx::BMPImageDecoderPlugin::create(file->bytes()).is_error());
}

TEST_CASE(test_bmp_v5_icc_profile_out_of_bounds)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("bmp/v5-icc-profile-out-of-bounds.bmp"sv)));
    EXPECT(Gfx::BMPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(file->bytes()));
    EXPECT(plugin_decoder->icc_data().is_error());
}

TEST_CASE(test_bmp_os2_3bit)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("bmp/os2_3bpc.bmp"sv)));
    EXPECT(Gfx::BMPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 300, 200 }));
    EXPECT_EQ(frame.image->get_pixel(150, 100), Gfx::Color::NamedColor::Black);
    EXPECT_EQ(frame.image->get_pixel(152, 100), Gfx::Color::NamedColor::White);
}

TEST_CASE(test_bmp_rle24)
{
    // 2x1 OS/2 2.x BI_RLE24 image: one run of two BGR (0x33, 0x22, 0x11) pixels.
    // Regression test for a misaligned u32 store in the RLE24 decoder (issue #9958).
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("bmp/rle24.bmp"sv)));
    EXPECT(Gfx::BMPImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 2, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0x11, 0x22, 0x33));
    EXPECT_EQ(frame.image->get_pixel(1, 0), Gfx::Color(0x11, 0x22, 0x33));
}

TEST_CASE(test_bmp_rle4_rows_with_odd_width)
{
    // 9x2 BI_RLE4 image. Each row is one run of two alternating colors, so every row ends mid-byte.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 40, 9, 2, 4, 2 /* BI_RLE4 */, 16);
    for (u8 i = 0; i < 16; ++i)
        append_u32(dib, i * 16 << 16);
    auto pixel_offset = dib.size();
    Array<u8, 8> rle_data { 9, 0x12, 0, 0, 9, 0x34, 0, 1 };
    dib.append(rle_data.data(), rle_data.size());
    auto bmp_data = make_bmp_file(dib, pixel_offset);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(bmp_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 9, 2 }));
    for (int x = 0; x < 9; ++x) {
        EXPECT_EQ(frame.image->get_pixel(x, 0), Gfx::Color(x % 2 ? 4 * 16 : 3 * 16, 0, 0));
        EXPECT_EQ(frame.image->get_pixel(x, 1), Gfx::Color(x % 2 ? 2 * 16 : 1 * 16, 0, 0));
    }
}

TEST_CASE(test_bmp_rgb32_has_no_alpha_channel)
{
    // 2x1 BI_RGB image. Without an alpha mask, the fourth byte of each pixel is not alpha.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 40, 2, 1, 32, 0 /* BI_RGB */, 0);
    auto pixel_offset = dib.size();
    Array<u8, 8> pixels { 0x33, 0x22, 0x11, 0x00, 0x66, 0x55, 0x44, 0x80 };
    dib.append(pixels.data(), pixels.size());
    auto bmp_data = make_bmp_file(dib, pixel_offset);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(bmp_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 2, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0x11, 0x22, 0x33));
    EXPECT_EQ(frame.image->get_pixel(1, 0), Gfx::Color(0x44, 0x55, 0x66));
}

TEST_CASE(test_bmp_v4_rgb16_with_empty_alpha_mask_is_opaque)
{
    // 1x1 BI_BITFIELDS RGB565 image with a BITMAPV4HEADER whose alpha mask is zero.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 108, 1, 1, 16, 3 /* BI_BITFIELDS */, 0);
    append_u32(dib, 0xf800); // Red mask.
    append_u32(dib, 0x07e0); // Green mask.
    append_u32(dib, 0x001f); // Blue mask.
    append_u32(dib, 0);      // Alpha mask.
    dib.resize(108);         // Color space, endpoints and gamma.
    auto pixel_offset = dib.size();
    append_u32(dib, 0xf800); // Red pixel, then row padding.
    auto bmp_data = make_bmp_file(dib, pixel_offset);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(bmp_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_bmp_palette_index_out_of_range_is_black)
{
    // 2x1 8-bit image with a one-color palette, whose second pixel uses an index past the end of the palette.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 40, 2, 1, 8, 0 /* BI_RGB */, 1);
    append_u32(dib, 0xff0000); // Red.
    auto pixel_offset = dib.size();
    Array<u8, 4> pixels { 0, 5, 0, 0 };
    dib.append(pixels.data(), pixels.size());
    auto bmp_data = make_bmp_file(dib, pixel_offset);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(bmp_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 2, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);
    EXPECT_EQ(frame.image->get_pixel(1, 0), Gfx::Color::NamedColor::Black);
}

TEST_CASE(test_bmp_ignores_plane_count)
{
    // 1x1 24-bit image whose plane count is 0 instead of 1.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 40, 1, 1, 24, 0 /* BI_RGB */, 0);
    dib[12] = 0;
    auto pixel_offset = dib.size();
    append_u32(dib, 0xff0000); // Red pixel in BGR order, then row padding.
    auto bmp_data = make_bmp_file(dib, pixel_offset);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(bmp_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_bmp_rle8_skipped_pixels_are_transparent)
{
    // 4x1 BI_RLE8 image: one red pixel, a delta that skips two pixels, then one green pixel.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 40, 4, 1, 8, 1 /* BI_RLE8 */, 3);
    append_u32(dib, 0x0000ff); // Blue.
    append_u32(dib, 0xff0000); // Red.
    append_u32(dib, 0x00ff00); // Green.
    auto pixel_offset = dib.size();
    Array<u8, 10> rle_data { 1, 1, 0, 2, 2, 0, 1, 2, 0, 1 };
    dib.append(rle_data.data(), rle_data.size());
    auto bmp_data = make_bmp_file(dib, pixel_offset);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(bmp_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 4, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);
    EXPECT_EQ(frame.image->get_pixel(1, 0).alpha(), 0);
    EXPECT_EQ(frame.image->get_pixel(2, 0).alpha(), 0);
    EXPECT_EQ(frame.image->get_pixel(3, 0), Gfx::Color::NamedColor::Green);
}

TEST_CASE(test_bmp_rle8_top_down)
{
    // 2x2 top-down BI_RLE8 image: a red row, then a row that ends after one green pixel.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 40, 2, -2, 8, 1 /* BI_RLE8 */, 2);
    append_u32(dib, 0xff0000); // Red.
    append_u32(dib, 0x00ff00); // Green.
    auto pixel_offset = dib.size();
    Array<u8, 8> rle_data { 2, 0, 0, 0, 1, 1, 0, 1 };
    dib.append(rle_data.data(), rle_data.size());
    auto bmp_data = make_bmp_file(dib, pixel_offset);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::BMPImageDecoderPlugin::create(bmp_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 2, 2 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);
    EXPECT_EQ(frame.image->get_pixel(1, 0), Gfx::Color::NamedColor::Red);
    EXPECT_EQ(frame.image->get_pixel(0, 1), Gfx::Color::NamedColor::Green);
    EXPECT_EQ(frame.image->get_pixel(1, 1).alpha(), 0);
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

TEST_CASE(test_ico_selects_largest_image_with_same_bpp)
{
    auto input = TEST_INPUT("ico/multiple-sizes-with-same-bpp.ico"sv);
    auto file = TRY_OR_FAIL(Core::MappedFile::map(input));

    auto plugin_decoder = TRY_OR_FAIL(Gfx::ICOImageDecoderPlugin::create(file->bytes()));
    EXPECT_EQ(plugin_decoder->size(), Gfx::IntSize(256, 256));
}

TEST_CASE(test_cur)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("cur/cursor.cur"sv)));
    EXPECT(Gfx::ICOImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::ICOImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 32, 32 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0, 0, 0, 0));
    EXPECT_EQ(frame.image->get_pixel(2, 2), Gfx::Color::NamedColor::Black);
    EXPECT_EQ(frame.image->get_pixel(8, 8), Gfx::Color::NamedColor::White);
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

TEST_CASE(test_gif_loop_count_from_netscape_extension)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("gif/loop-count-2.gif"sv)));
    EXPECT(Gfx::GIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::GIFImageDecoderPlugin::create(file->bytes()));

    EXPECT_EQ(plugin_decoder->frame_count(), 2u);
    EXPECT(plugin_decoder->is_animated());

    EXPECT_EQ(plugin_decoder->loop_count(), 3u);
}

TEST_CASE(test_gif_corrupt_second_frame)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("gif/corrupt-second-frame.gif"sv)));
    EXPECT(Gfx::GIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::GIFImageDecoderPlugin::create(file->bytes()));

    EXPECT_EQ(plugin_decoder->frame_count(), 2u);

    auto first_frame = TRY_OR_FAIL(plugin_decoder->frame(0));
    EXPECT_EQ(first_frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);

    EXPECT(plugin_decoder->frame(1).is_error());
    EXPECT(!plugin_decoder->frame(0).is_error());
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
    EXPECT_EQ(frame.image->size(), Gfx::IntSize(1, 1));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);
}

TEST_CASE(test_gif_empty_lzw_data)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("gif/minimal-1x1.gif"sv)));
    EXPECT(Gfx::GIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::GIFImageDecoderPlugin::create(file->bytes()));

    EXPECT_EQ(plugin_decoder->frame_count(), 1u);
    auto frame = TRY_OR_FAIL(plugin_decoder->frame(0));
    EXPECT_EQ(frame.image->size(), Gfx::IntSize(1, 1));
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

TEST_CASE(test_24bit_bmp_embedded_in_ico)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("ico/yt-favicon.ico"sv)));
    EXPECT(Gfx::ICOImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::ICOImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 16, 16 }));
    EXPECT_EQ(frame.image->get_pixel(14, 14), Gfx::Color(234, 0, 0));
    EXPECT_EQ(frame.image->get_pixel(13, 15), Gfx::Color(255, 10, 15));
}

TEST_CASE(test_32bit_bmp_without_alpha_values_in_ico)
{
    // 2x1 image whose fourth bytes are all zero, so only its AND mask makes pixels transparent.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 40, 2, 2, 32, 0 /* BI_RGB */, 0);
    Array<u8, 12> pixels_and_mask { 0x33, 0x22, 0x11, 0x00, 0x66, 0x55, 0x44, 0x00, 0b0100'0000, 0, 0, 0 };
    dib.append(pixels_and_mask.data(), pixels_and_mask.size());
    auto ico_data = make_ico_file(2, 1, 32, dib);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::ICOImageDecoderPlugin::create(ico_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 2, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0x11, 0x22, 0x33));
    EXPECT_EQ(frame.image->get_pixel(1, 0).alpha(), 0);
}

TEST_CASE(test_32bit_bmp_with_alpha_values_in_ico_ignores_and_mask)
{
    // 2x1 image with alpha values, followed by an AND mask that would make both pixels transparent.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 40, 2, 2, 32, 0 /* BI_RGB */, 0);
    Array<u8, 12> pixels_and_mask { 0x33, 0x22, 0x11, 0x80, 0x66, 0x55, 0x44, 0xff, 0b1100'0000, 0, 0, 0 };
    dib.append(pixels_and_mask.data(), pixels_and_mask.size());
    auto ico_data = make_ico_file(2, 1, 32, dib);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::ICOImageDecoderPlugin::create(ico_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 2, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0x11, 0x22, 0x33, 0x80));
    EXPECT_EQ(frame.image->get_pixel(1, 0), Gfx::Color(0x44, 0x55, 0x66));
}

TEST_CASE(test_bitfields_bmp_with_alpha_values_in_ico_ignores_and_mask)
{
    // 1x1 image with an alpha mask and an alpha value, followed by an AND mask that would make the pixel transparent.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 56, 1, 2, 32, 3 /* BI_BITFIELDS */, 0);
    append_u32(dib, 0x00ff0000);
    append_u32(dib, 0x0000ff00);
    append_u32(dib, 0x000000ff);
    append_u32(dib, 0xff000000);
    Array<u8, 8> pixel_and_mask { 0x33, 0x22, 0x11, 0x80, 0b1000'0000, 0, 0, 0 };
    dib.append(pixel_and_mask.data(), pixel_and_mask.size());
    auto ico_data = make_ico_file(1, 1, 32, dib);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::ICOImageDecoderPlugin::create(ico_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(0x11, 0x22, 0x33, 0x80));
}

TEST_CASE(test_rle8_bmp_in_ico_uses_and_mask)
{
    // 8x1 image whose RLE data sets the first four pixels and skips the rest, followed by an AND mask that makes the
    // first pixel transparent.
    Vector<u8> dib;
    append_bitmap_info_header(dib, 40, 8, 2, 8, 1 /* BI_RLE8 */, 1);
    append_u32(dib, 0xff0000); // Red.
    Array<u8, 8> rle_data_and_mask { 4, 0, 0, 1, 0b1000'0000, 0, 0, 0 };
    dib.append(rle_data_and_mask.data(), rle_data_and_mask.size());
    auto ico_data = make_ico_file(8, 1, 8, dib);

    auto plugin_decoder = TRY_OR_FAIL(Gfx::ICOImageDecoderPlugin::create(ico_data));
    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 8, 1 }));
    EXPECT_EQ(frame.image->get_pixel(0, 0).alpha(), 0);
    EXPECT_EQ(frame.image->get_pixel(1, 0), Gfx::Color::NamedColor::Red);
    EXPECT_EQ(frame.image->get_pixel(3, 0), Gfx::Color::NamedColor::Red);
    EXPECT_EQ(frame.image->get_pixel(4, 0).alpha(), 0);
    EXPECT_EQ(frame.image->get_pixel(7, 0).alpha(), 0);
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

TEST_CASE(test_jpeg_cmyk_no_adobe_marker)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/cmyk-no-adobe-marker.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));
    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 10, 10 }));

    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(8, 1), Gfx::Color(44, 184, 97));
    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(1, 8), Gfx::Color(184, 44, 97));
    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(9, 9), Gfx::Color(24, 24, 194));
}

TEST_CASE(test_jpeg_cmyk_truncated)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/cmyk-no-adobe-marker.jpg"sv)));
    auto truncated_file = file->bytes().slice(0, 450);
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(truncated_file));
    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 10, 10 }));

    auto image = plugin_decoder->frame(0).value().image;
    EXPECT_EQ(image->get_pixel(8, 1), Gfx::Color(44, 184, 97));
    for (int y = 8; y < 10; ++y) {
        for (int x = 0; x < 10; ++x)
            EXPECT_EQ(image->get_pixel(x, y), Gfx::Color(0, 0, 0));
    }
}

TEST_CASE(test_jpeg_cmyk_adobe_transform_0)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/cmyk-adobe-transform-0.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));
    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 10, 10 }));

    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(8, 1), Gfx::Color(44, 184, 97));
    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(1, 8), Gfx::Color(184, 44, 97));
    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(9, 9), Gfx::Color(24, 24, 194));
}

TEST_CASE(test_jpeg_ycck_adobe_transform_2)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/ycck-adobe-transform-2.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));
    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 10, 10 }));

    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(8, 1), Gfx::Color(197, 27, 134));
    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(1, 8), Gfx::Color(24, 199, 134));
    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(9, 9), Gfx::Color(227, 224, 19));
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

TEST_CASE(test_jpeg_sof2_successive_approximation)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/successive_approximation.jpg"sv)));
    EXPECT(Gfx::JPEGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 600, 800 }));
}

TEST_CASE(test_jpeg_sof2_truncated)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/successive_approximation.jpg"sv)));
    auto full_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));
    auto full_image = TRY_OR_FAIL(full_decoder->frame(0)).image;

    // Cut off within the later scans, which only refine the image the earlier ones already cover.
    auto truncated_file = file->bytes().slice(0, file->bytes().size() * 6 / 10);
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(truncated_file));
    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 600, 800 }));

    auto image = plugin_decoder->frame(0).value().image;
    for (auto point : { Gfx::IntPoint { 100, 100 }, Gfx::IntPoint { 300, 400 }, Gfx::IntPoint { 500, 700 } }) {
        auto pixel = image->get_pixel(point.x(), point.y());
        auto full_pixel = full_image->get_pixel(point.x(), point.y());
        EXPECT(abs(pixel.red() - full_pixel.red()) < 48);
        EXPECT(abs(pixel.green() - full_pixel.green()) < 48);
        EXPECT(abs(pixel.blue() - full_pixel.blue()) < 48);
    }
}

TEST_CASE(test_jpeg_sof2_truncated_after_first_scan)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jpg/successive_approximation.jpg"sv)));
    auto bytes = file->bytes();
    auto full_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(bytes));
    auto full_image = TRY_OR_FAIL(full_decoder->frame(0)).image;

    // Cut off where the second scan's SOS marker starts, so only the first scan arrives, but all of it.
    size_t start_of_scan_markers = 0;
    size_t cut = 0;
    for (size_t i = 0; i + 1 < bytes.size(); ++i) {
        if (bytes[i] == 0xFF && bytes[i + 1] == 0xDA && ++start_of_scan_markers == 2) {
            cut = i;
            break;
        }
    }
    EXPECT(cut > 0);
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGImageDecoderPlugin::create(bytes.slice(0, cut)));
    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 600, 800 }));

    // Every row comes out, the bottom ones too, as a coarser version of the full image.
    auto image = plugin_decoder->frame(0).value().image;
    for (int y = 790; y < 800; ++y) {
        auto pixel = image->get_pixel(300, y);
        auto full_pixel = full_image->get_pixel(300, y);
        EXPECT(abs(pixel.red() - full_pixel.red()) < 64);
        EXPECT(abs(pixel.green() - full_pixel.green()) < 64);
        EXPECT(abs(pixel.blue() - full_pixel.blue()) < 64);
    }
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

TEST_CASE(test_png_truncated)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("png/1x65535.png"sv)));
    auto bytes = file->bytes().slice(0, file->bytes().size() / 2);
    auto decoder = TRY_OR_FAIL(Gfx::PNGImageDecoderPlugin::create(bytes));
    auto frame = TRY_OR_FAIL(decoder->frame(0));
    EXPECT_EQ(frame.image->size(), Gfx::IntSize(1, 65535));
    EXPECT_EQ(frame.image->get_pixel(0, 16384), Gfx::Color::NamedColor::Red);
    EXPECT_EQ(frame.image->get_pixel(0, 65534), Gfx::Color::NamedColor::Transparent);
}

TEST_CASE(test_apng)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("png/apng-1-frame.png"sv)));
    EXPECT(Gfx::PNGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::PNGImageDecoderPlugin::create(file->bytes()));

    EXPECT_EQ(plugin_decoder->frame_count(), 1u);
    EXPECT_EQ(plugin_decoder->loop_count(), 0u);

    auto frame = TRY_OR_FAIL(plugin_decoder->frame(0));

    EXPECT_EQ(frame.duration, 1000);
    EXPECT_EQ(frame.image->get_pixel(64, 32), Gfx::Color(117, 252, 76));
    EXPECT_EQ(frame.image->size(), Gfx::IntSize(128, 64));
}

TEST_CASE(test_apng_idat_not_affecting_next_frame)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("png/apng-blend.png"sv)));
    EXPECT(Gfx::PNGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::PNGImageDecoderPlugin::create(file->bytes()));

    EXPECT_EQ(plugin_decoder->frame_count(), 1u);
    EXPECT_EQ(plugin_decoder->loop_count(), 0u);

    auto frame = TRY_OR_FAIL(plugin_decoder->frame(0));

    EXPECT_EQ(frame.duration, 1000);
    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color::NamedColor::Transparent);
    EXPECT_EQ(frame.image->size(), Gfx::IntSize(100, 100));
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

TEST_CASE(test_exif_orientation_transpose_non_square)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("png/exif-orientation-5.png"sv)));
    EXPECT(Gfx::PNGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::PNGImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1, 3 }));
    EXPECT(plugin_decoder->metadata().has_value());
    auto const& exif_metadata = static_cast<Gfx::ExifMetadata const&>(plugin_decoder->metadata().value());
    EXPECT_EQ(*exif_metadata.orientation(), Gfx::TIFF::Orientation::Rotate90ClockwiseThenFlipHorizontally);

    EXPECT_EQ(frame.image->get_pixel(0, 0), Gfx::Color(255, 0, 0));
    EXPECT_EQ(frame.image->get_pixel(0, 1), Gfx::Color(0, 255, 0));
    EXPECT_EQ(frame.image->get_pixel(0, 2), Gfx::Color(0, 0, 255));
}

TEST_CASE(test_exif_sub_ifd_chain)
{
    // Every IFD sets an orientation and points to the next one with an ExifIFD tag. Only the main IFD and the first
    // sub-IFD are read, and the chain must not exhaust the stack.
    static constexpr u32 ifd_count = 100'000;
    static constexpr u32 ifd_size = 2 + 2 * 12 + 4;

    ByteBuffer exif;
    auto append_u16 = [&](u16 value) { exif.append(&value, sizeof(value)); };
    auto append_u32 = [&](u32 value) { exif.append(&value, sizeof(value)); };

    append_u16(0x4949);
    append_u16(42);
    append_u32(8);
    for (u32 i = 0; i < ifd_count; ++i) {
        append_u16(2);

        // Orientation, Short, count 1.
        append_u16(274);
        append_u16(3);
        append_u32(1);
        append_u16(i == 0 ? 1 : (i == 1 ? 6 : 3));
        append_u16(0);

        // ExifIFD, UnsignedLong, count 1.
        append_u16(34665);
        append_u16(4);
        append_u32(1);
        append_u32(i + 1 < ifd_count ? 8 + (i + 1) * ifd_size : 0);

        append_u32(0);
    }

    auto metadata = TRY_OR_FAIL(Gfx::TIFF::read_exif_metadata(exif));
    EXPECT_EQ(*metadata->orientation(), Gfx::TIFF::Orientation::Rotate90Clockwise);
}

TEST_CASE(test_exif_repeated_sub_ifd_tag)
{
    // The main IFD points to two sub-IFDs that set different orientations. Only the first one is read.
    ByteBuffer exif;
    auto append_u16 = [&](u16 value) { exif.append(&value, sizeof(value)); };
    auto append_u32 = [&](u32 value) { exif.append(&value, sizeof(value)); };
    auto append_orientation_ifd = [&](u16 orientation) {
        append_u16(1);
        append_u16(274);
        append_u16(3);
        append_u32(1);
        append_u16(orientation);
        append_u16(0);
        append_u32(0);
    };

    static constexpr u32 main_ifd_size = 2 + 2 * 12 + 4;
    static constexpr u32 orientation_ifd_size = 2 + 12 + 4;

    append_u16(0x4949);
    append_u16(42);
    append_u32(8);

    append_u16(2);
    for (u32 i = 0; i < 2; ++i) {
        append_u16(34665);
        append_u16(4);
        append_u32(1);
        append_u32(8 + main_ifd_size + i * orientation_ifd_size);
    }
    append_u32(0);

    append_orientation_ifd(6);
    append_orientation_ifd(3);

    auto metadata = TRY_OR_FAIL(Gfx::TIFF::read_exif_metadata(exif));
    EXPECT_EQ(*metadata->orientation(), Gfx::TIFF::Orientation::Rotate90Clockwise);
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

// Regression test: libpng longjmp() out of png_read_image() must not leak the in-flight row-pointers buffer or bitmap
// from PNGLoadingContext::read_frames(). The fixture has a valid IHDR, but a corrupted IDAT body that decompresses past
// its zlib end-of-block marker — forcing libpng to longjmp mid-decode.
TEST_CASE(test_png_corrupt_idat_does_not_leak_on_libpng_longjmp)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("png/corrupt-idat.png"sv)));
    auto plugin_or_error = Gfx::PNGImageDecoderPlugin::create(file->bytes());
    // PNGImageDecoderPlugin::create() catches the libpng error and falls back to a single-frame placeholder — so we
    // don't assert on the result here. The test passes if the run completes without LeakSanitizer reports.
    if (!plugin_or_error.is_error())
        (void)plugin_or_error.release_value()->frame(0);
}

TEST_CASE(test_png_large_dimensions)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("png/65535x1.png"sv)));
    EXPECT(Gfx::PNGImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::PNGImageDecoderPlugin::create(file->bytes()));
    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 65535, 1 }));
    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);

    file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("png/1x65535.png"sv)));
    EXPECT(Gfx::PNGImageDecoderPlugin::sniff(file->bytes()));
    plugin_decoder = TRY_OR_FAIL(Gfx::PNGImageDecoderPlugin::create(file->bytes()));
    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 1, 65535 }));
    EXPECT_EQ(plugin_decoder->frame(0).value().image->get_pixel(0, 0), Gfx::Color::NamedColor::Red);
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

TEST_CASE(test_webp_extended_missing_declared_icc_chunk)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("webp/simple-vp8.webp"sv)));
    auto malformed_webp = TRY_OR_FAIL(make_webp_with_declared_but_missing_iccp_chunk(file->bytes()));

    EXPECT(Gfx::WebPImageDecoderPlugin::sniff(malformed_webp));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::WebPImageDecoderPlugin::create(malformed_webp));
    EXPECT(!TRY_OR_FAIL(plugin_decoder->icc_data()).has_value());

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 240, 240 }));
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
    // In addition to testing the index transform, this file also tests handling of explicitly setting max_symbol.
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

TEST_CASE(test_jxl_modular_simple_tree_upsample2_10bits)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jxl/modular_simple_tree_upsample2_10bits_rct.jxl"sv)));
    EXPECT(Gfx::JPEGXLImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGXLImageDecoderPlugin::create(file->bytes()));

    TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 128, 128 }));

    auto frame = TRY_OR_FAIL(plugin_decoder->frame(0));
}

TEST_CASE(test_jxl_truncated_frame_is_not_counted)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("jxl/modular_simple_tree_upsample2_10bits_rct.jxl"sv)));
    auto truncated_file = file->bytes().slice(0, file->bytes().size() - 1);
    EXPECT(Gfx::JPEGXLImageDecoderPlugin::sniff(truncated_file));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::JPEGXLImageDecoderPlugin::create(truncated_file));

    EXPECT_EQ(plugin_decoder->frame_count(), 0u);
    EXPECT(plugin_decoder->frame(0).is_error());
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
    EXPECT(Gfx::AVIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::AVIFImageDecoderPlugin::create(file->bytes()));

    auto frame = TRY_OR_FAIL(expect_single_frame_of_size(*plugin_decoder, { 240, 240 }));

    // While AVIF YUV contents are defined bit-exact, the YUV->RGB conversion isn't.
    // So pixels changing by 1 or so below is fine if you change code.
    EXPECT_EQ(frame.image->get_pixel(120, 232), Gfx::Color(0xf1, 0xef, 0xf0, 255));
    EXPECT_EQ(frame.image->get_pixel(198, 202), Gfx::Color(0x79, 0xab, 0xd6, 255));
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

TEST_CASE(test_avif_animated)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("loop_forever.avif"sv)));
    EXPECT(Gfx::AVIFImageDecoderPlugin::sniff(file->bytes()));
    auto plugin_decoder = TRY_OR_FAIL(Gfx::AVIFImageDecoderPlugin::create(file->bytes()));

    EXPECT(plugin_decoder->is_animated());
    EXPECT_EQ(plugin_decoder->frame_count(), 2u);
    EXPECT_EQ(plugin_decoder->loop_count(), 0u);

    // Durations are known before any frame is decoded.
    EXPECT_EQ(plugin_decoder->frame_duration(0), 500);
    EXPECT_EQ(plugin_decoder->frame_duration(1), 500);

    // Frames can be decoded out of order and more than once.
    auto second_frame = TRY_OR_FAIL(plugin_decoder->frame(1));
    EXPECT_EQ(second_frame.image->size(), Gfx::IntSize(128, 128));
    EXPECT_EQ(second_frame.duration, 500);
    EXPECT_EQ(second_frame.image->get_pixel(64, 64), Gfx::Color(253, 253, 255));

    auto first_frame = TRY_OR_FAIL(plugin_decoder->frame(0));
    EXPECT_EQ(first_frame.duration, 500);
    EXPECT_EQ(first_frame.image->get_pixel(64, 64), Gfx::Color(1, 0, 0));

    auto second_frame_again = TRY_OR_FAIL(plugin_decoder->frame(1));
    EXPECT_EQ(second_frame_again.image->get_pixel(64, 64), Gfx::Color(253, 253, 255));

    EXPECT(plugin_decoder->frame(2).is_error());
}

TEST_CASE(test_avif_missing_pixi_property)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map(TEST_INPUT("avif/missing-pixi-property.avif"sv)));
    EXPECT(Gfx::AVIFImageDecoderPlugin::sniff(file->bytes()));
}
