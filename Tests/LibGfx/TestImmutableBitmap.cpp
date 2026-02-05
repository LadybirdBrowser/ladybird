/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Try.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibTest/TestCase.h>

TEST_CASE(export_to_byte_buffer)
{
    enum class Premultiplied : u8 {
        Yes,
        No,
    };

    struct TestData {
        Vector<Gfx::BitmapFormat> source_formats_to_test;
        Vector<Premultiplied> source_alpha_cases_to_test;
        Gfx::BGRA8888 source_pixels[4];
        Gfx::ExportFormat export_format;
        Vector<Premultiplied> target_alpha_cases_to_test;
        Vector<u8> expected_result;
    };

    Vector<Gfx::BitmapFormat> all_bitmap_formats = {
        Gfx::BitmapFormat::BGRx8888,
        Gfx::BitmapFormat::BGRA8888,
        Gfx::BitmapFormat::RGBx8888,
        Gfx::BitmapFormat::RGBA8888,
    };

    Vector<Gfx::BitmapFormat> alpha_bitmap_formats = {
        Gfx::BitmapFormat::BGRA8888,
        Gfx::BitmapFormat::RGBA8888,
    };

    Vector<Gfx::BitmapFormat> non_alpha_bitmap_formats = {
        Gfx::BitmapFormat::BGRx8888,
        Gfx::BitmapFormat::RGBx8888,
    };

    // FIXME: Some of these test cases seem suspect, particularly with regard to alpha-(un)premultiplication. We should
    // validate whether these actually have the correct behavior.
    TestData subtests[] = {
        {
            alpha_bitmap_formats,
            { Premultiplied::No },
            { 0x00FFFFFF, 0x55FFFFFF, 0xAAFFFFFF, 0xFFFFFFFF },
            Gfx::ExportFormat::Gray8,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00, 0x55, 0xAA, 0xFF },
        },
        {
            non_alpha_bitmap_formats,
            { Premultiplied::No },
            { 0x00000000, 0x55555555, 0xAAAAAAAA, 0xFFFFFFFF },
            Gfx::ExportFormat::Gray8,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00, 0x55, 0xAA, 0xFF },
        },
        {
            all_bitmap_formats,
            { Premultiplied::Yes },
            { 0x00000000, 0x55555555, 0xAAAAAAAA, 0xFFFFFFFF },
            Gfx::ExportFormat::Gray8,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00, 0x55, 0xAA, 0xFF },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF },
            Gfx::ExportFormat::Alpha8,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00, 0x44, 0x88, 0xCC },
        },
        {
            non_alpha_bitmap_formats,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF },
            Gfx::ExportFormat::Alpha8,
            { Premultiplied::No, Premultiplied::Yes },
            { 0xFF, 0xFF, 0xFF, 0xFF },
        },
        {
            non_alpha_bitmap_formats,
            { Premultiplied::No, Premultiplied::Yes },
            { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFF00FF },
            Gfx::ExportFormat::RGB565,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0x1F, 0xF8 },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::No },
            { 0x33FFFFFF, 0x66FFFFFF, 0x99FFFFFF, 0xCCFFFFFF },
            Gfx::ExportFormat::RGB565,
            { Premultiplied::No, Premultiplied::Yes },
            { 0xA6, 0x31, 0x2C, 0x63, 0xD3, 0x9C, 0x59, 0xCE },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::Yes },
            { 0x33FF0000, 0x6600FF00, 0x990000FF, 0xCCFF00FF },
            Gfx::ExportFormat::RGB565,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0x1F, 0xF8 },
        },
        {
            non_alpha_bitmap_formats,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF },
            Gfx::ExportFormat::RGBA4444,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x3f, 0x12, 0x7f, 0x56, 0xBF, 0x9A, 0xFF, 0xDE },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::No },
            { 0x33001122, 0x77445566, 0xBB8899AA, 0xFFCCDDEE },
            Gfx::ExportFormat::RGBA4444,
            { Premultiplied::No },
            { 0x23, 0x01, 0x67, 0x45, 0xAB, 0x89, 0xEF, 0xCD },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::No },
            { 0x3355AAFF, 0x6655AAFF, 0x9955AAFF, 0xCC55AAFF },
            Gfx::ExportFormat::RGBA4444,
            { Premultiplied::Yes },
            { 0x33, 0x12, 0x66, 0x24, 0x99, 0x36, 0xCC, 0x48 },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::Yes },
            { 0x33112233, 0x66224466, 0x99336699, 0xCC4488CC },
            Gfx::ExportFormat::RGBA4444,
            { Premultiplied::No },
            { 0xF3, 0x5A, 0xF6, 0x5A, 0xF9, 0x5A, 0xFC, 0x5A },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::Yes },
            { 0x33001122, 0x77445566, 0xBB8899AA, 0xFFCCDDEE },
            Gfx::ExportFormat::RGBA4444,
            { Premultiplied::Yes },
            { 0x23, 0x01, 0x67, 0x45, 0xAB, 0x89, 0xEF, 0xCD },
        },
        {
            all_bitmap_formats,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF },
            Gfx::ExportFormat::RGB888,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x11, 0x22, 0x33, 0x55, 0x66, 0x77, 0x99, 0xAA, 0xBB, 0xDD, 0xEE, 0xFF },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::No },
            { 0x33001122, 0x77445566, 0xBB8899AA, 0xFFCCDDEE },
            Gfx::ExportFormat::RGBA8888,
            { Premultiplied::No },
            { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::No },
            { 0x3355AAFF, 0x6655AAFF, 0x9955AAFF, 0xCC55AAFF },
            Gfx::ExportFormat::RGBA8888,
            { Premultiplied::Yes },
            { 0x11, 0x22, 0x33, 0x33, 0x22, 0x44, 0x66, 0x66, 0x33, 0x66, 0x99, 0x99, 0x44, 0x88, 0xCC, 0xCC },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::Yes },
            { 0x33112233, 0x66224466, 0x99336699, 0xCC4488CC },
            Gfx::ExportFormat::RGBA8888,
            { Premultiplied::No },
            { 0x55, 0xAA, 0xFF, 0x33, 0x55, 0xAA, 0xFF, 0x66, 0x55, 0xAA, 0xFF, 0x99, 0x55, 0xAA, 0xFF, 0xCC },
        },
        {
            alpha_bitmap_formats,
            { Premultiplied::Yes },
            { 0x33001122, 0x77445566, 0xBB8899AA, 0xFFCCDDEE },
            Gfx::ExportFormat::RGBA8888,
            { Premultiplied::Yes },
            { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF },
        },
        {
            non_alpha_bitmap_formats,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF },
            Gfx::ExportFormat::RGBA8888,
            { Premultiplied::No, Premultiplied::Yes },
            { 0x11, 0x22, 0x33, 0xFF, 0x55, 0x66, 0x77, 0xFF, 0x99, 0xAA, 0xBB, 0xFF, 0xDD, 0xEE, 0xFF, 0xFF },
        },
    };

    auto alpha_case_name = [](Premultiplied premultiplied) -> StringView {
        return premultiplied == Premultiplied::Yes ? "premultiplied"sv : "unpremultiplied"sv;
    };

    auto flip_y_name = [](u32 flags) -> StringView {
        return flags & Gfx::ExportFlags::FlipY ? "flip Y"sv : "keep Y"sv;
    };

    auto count = 0;
    for (auto const& subtest : subtests) {
        for (auto source_format : subtest.source_formats_to_test) {
            for (auto maybe_flip_y_flag : Vector<int> { 0, Gfx::ExportFlags::FlipY }) {
                for (auto source_alpha_case : subtest.source_alpha_cases_to_test) {
                    for (auto target_alpha_case : subtest.target_alpha_cases_to_test) {
                        auto export_flags = 0;
                        export_flags |= maybe_flip_y_flag;
                        export_flags |= target_alpha_case == Premultiplied::Yes ? Gfx::ExportFlags::PremultiplyAlpha : 0;

                        dbgln("Running subtest {}: {} -> {}, {} -> {}, {}", count++, bitmap_format_name(source_format), export_format_name(subtest.export_format), alpha_case_name(source_alpha_case), alpha_case_name(target_alpha_case), flip_y_name(export_flags));

                        auto source_alpha_type = source_alpha_case == Premultiplied::Yes ? Gfx::AlphaType::Premultiplied : Gfx::AlphaType::Unpremultiplied;
                        auto bitmap = MUST(Gfx::Bitmap::create(source_format, source_alpha_type, { 2, 2 }));
                        auto logical_y0 = maybe_flip_y_flag ? 1 : 0;
                        auto logical_y1 = maybe_flip_y_flag ? 0 : 1;
                        bitmap->set_pixel(0, logical_y0, Color::from_bgra(subtest.source_pixels[0]));
                        bitmap->set_pixel(1, logical_y0, Color::from_bgra(subtest.source_pixels[1]));
                        bitmap->set_pixel(0, logical_y1, Color::from_bgra(subtest.source_pixels[2]));
                        bitmap->set_pixel(1, logical_y1, Color::from_bgra(subtest.source_pixels[3]));

                        auto immutable_bitmap = Gfx::ImmutableBitmap::create(bitmap);
                        auto result = MUST(immutable_bitmap->export_to_byte_buffer(subtest.export_format, export_flags, 2, 2));

                        EXPECT_EQ(result.width, 2);
                        EXPECT_EQ(result.height, 2);
                        EXPECT_EQ(result.buffer.bytes(), subtest.expected_result);
                    }
                }
            }
        }
    }
}
