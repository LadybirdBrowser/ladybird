/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/ElapsedTimer.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>

#include <core/SkBitmap.h>
#include <core/SkCanvas.h>
#include <core/SkColorFilter.h>
#include <core/SkImage.h>
#include <core/SkPaint.h>
#include <core/SkSamplingOptions.h>
#include <core/SkSurface.h>

// Times the shipped force-dark image filter on the CPU raster pipeline.

static constexpr int image_side = 1024;
static constexpr int draw_iterations = 20;

static sk_sp<SkImage> make_varied_test_image()
{
    SkBitmap bitmap;
    bitmap.allocN32Pixels(image_side, image_side);
    // A deterministic spread of colors — so the filter's data-dependent branches (the sRGB transfer split, the gray
    // snap) all get exercised — rather than one path being measured for the whole surface.
    u32 state = 0x12345678;
    for (int y = 0; y < image_side; ++y) {
        for (int x = 0; x < image_side; ++x) {
            state = state * 1664525 + 1013904223;
            *bitmap.getAddr32(x, y) = state | 0xff000000;
        }
    }
    bitmap.setImmutable();
    return bitmap.asImage();
}

BENCHMARK_CASE(force_dark_image_filter_per_pixel)
{
    auto image = make_varied_test_image();
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(image_side, image_side));
    VERIFY(surface);
    auto* canvas = surface->getCanvas();

    // The unfiltered pass takes a few milliseconds, which the coarse clock's tick would swallow; the filtered pass is
    // measured the same way so the two are comparable.
    SkPaint unfiltered;
    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    for (int i = 0; i < draw_iterations; ++i)
        canvas->drawImage(image, 0, 0, SkSamplingOptions {}, &unfiltered);
    auto unfiltered_ms = timer.elapsed_milliseconds();

    SkPaint filtered;
    filtered.setColorFilter(Web::Painting::force_dark_image_color_filter());
    timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    for (int i = 0; i < draw_iterations; ++i)
        canvas->drawImage(image, 0, 0, SkSamplingOptions {}, &filtered);
    auto filtered_ms = timer.elapsed_milliseconds();

    auto pixels = static_cast<double>(image_side) * image_side * draw_iterations;
    auto per_pixel_ns = (static_cast<double>(filtered_ms - unfiltered_ms) * 1'000'000.0) / pixels;
    outln("unfiltered {} ms, filtered {} ms over {} draws of {}x{}: {:.2f} ns per filtered pixel",
        unfiltered_ms, filtered_ms, draw_iterations, image_side, image_side, per_pixel_ns);
}
