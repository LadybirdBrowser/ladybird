/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/DisplayListPlayerSkia.h>
#include <Compositor/DisplayListRasterCache.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/SkiaUtils.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibGfx/Bitmap.h>
#include <LibTest/TestCase.h>

#include <core/SkBlender.h>
#include <core/SkCanvas.h>
#include <core/SkPaint.h>
#include <core/SkPath.h>

TEST_CASE(zero_width_border_sides_do_not_paint_on_gpu_surface)
{
    // Set up the GPU renderer and pixel readback.
    auto context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
    if (!context) {
        warnln("No GPU backend available, skipping");
        return;
    }

    Gfx::IntSize size { 10, 10 };
    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, context);
    auto tree = Compositing::VisualContextTreeTestBuilder {}.finish();
    Compositor::DisplayListPlayerSkia player { context };
    Compositing::DisplayListResourceStorage storage;
    Compositor::DisplayListRasterCache cache;
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));

    // A half-pixel offset must not make the zero-width top and left borders visible.
    auto& canvas = surface->canvas();
    canvas.clear(SK_ColorWHITE);
    canvas.translate(0.5f, 0.5f);

    Compositing::FillRoundedRectRing ring {
        .rect = { 0, 0, 8, 8 },
        .corner_radii = {},
        .top_width = 0,
        .right_width = 1,
        .bottom_width = 1,
        .left_width = 0,
        .color = Gfx::Color::Black,
    };
    auto list = Compositing::VisualContextTreeTestBuilder::rounded_rect_ring_display_list(tree, {}, ring);
    player.execute(*list, tree, storage, cache, {}, surface, nullptr, nullptr);
    surface->read_into_bitmap(*bitmap);

    EXPECT_EQ(bitmap->get_pixel(0, 4), Gfx::Color::White); // Left border.
    EXPECT_EQ(bitmap->get_pixel(4, 0), Gfx::Color::White); // Top border.
    EXPECT_NE(bitmap->get_pixel(8, 4), Gfx::Color::White); // Right border.
}

static int count_red_pixels(Gfx::Bitmap const& bitmap, Gfx::IntRect const& rect)
{
    int count = 0;
    for (int y = rect.top(); y < rect.bottom(); ++y) {
        for (int x = rect.left(); x < rect.right(); ++x) {
            auto color = bitmap.get_pixel(x, y);
            if (color.red() > 128 && color.green() < 64 && color.blue() < 64)
                ++count;
        }
    }
    return count;
}

TEST_CASE(stroke_then_blend_layer_on_gpu_surface)
{
    // A stroked path selects multisampling on a GPU surface, and a layer that blends with its backdrop reads the
    // destination in the same surface. Both must survive in the same frame with a layer between them.

    auto context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
    if (!context) {
        warnln("No GPU backend available, skipping");
        return;
    }

    Gfx::IntSize size { 200, 100 };
    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, context);
    auto& canvas = surface->canvas();
    canvas.clear(SK_ColorBLACK);

    Gfx::IntRect box_rect { 120, 20, 60, 60 };
    SkPaint box_paint;
    box_paint.setColor(SK_ColorRED);
    canvas.drawRect(SkRect::MakeXYWH(box_rect.x(), box_rect.y(), box_rect.width(), box_rect.height()), box_paint);

    SkPaint stroke_paint;
    stroke_paint.setColor(SK_ColorRED);
    stroke_paint.setAntiAlias(true);
    stroke_paint.setStyle(SkPaint::kStroke_Style);
    stroke_paint.setStrokeWidth(4);
    canvas.drawPath(SkPath::Circle(50, 50, 20), stroke_paint);

    SkPaint layer_paint;
    layer_paint.setBlender(Gfx::to_skia_blender(Gfx::CompositingAndBlendingOperator::Overlay));
    canvas.saveLayer(nullptr, &layer_paint);
    SkPaint gray_paint;
    gray_paint.setColor(SkColorSetARGB(255, 0x80, 0x80, 0x80));
    canvas.drawRect(SkRect::MakeWH(100, 100), gray_paint);
    canvas.restore();

    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
    surface->read_into_bitmap(*bitmap);

    EXPECT_EQ(count_red_pixels(*bitmap, box_rect), box_rect.width() * box_rect.height());
    EXPECT(count_red_pixels(*bitmap, { 26, 26, 48, 48 }) > 0);
}

TEST_CASE(flush_surface_skia_allocated)
{
    // Only a surface wrapping an image shared with another process is presented when flushed. One Skia allocated
    // itself has no memory to present if allocating it failed during the flush, so it is flushed and read back.

    auto cpu_surface = Gfx::PaintingSurface::create_with_size({ 1, 1 }, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
    EXPECT(!cpu_surface->wraps_shared_image());

    auto context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
    if (!context) {
        warnln("No GPU backend available, skipping");
        return;
    }

    Gfx::IntSize size { 20, 10 };
    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, context);
    EXPECT(!surface->wraps_shared_image());

    bool flushed = false;
    surface->canvas().clear(SK_ColorRED);
    context->flush_and_submit_async(*surface, [&] { flushed = true; });
    surface->canvas().clear(SK_ColorRED);
    context->flush_and_submit(*surface);
    EXPECT(flushed);

    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
    surface->read_into_bitmap(*bitmap);
    EXPECT_EQ(count_red_pixels(*bitmap, { {}, size }), size.width() * size.height());
}

TEST_CASE(surface_too_wide_for_a_bitmap_is_an_error)
{
    auto surface = Gfx::PaintingSurface::try_create_with_size({ 70000, 1 }, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
    EXPECT(surface.is_error());
}

TEST_CASE(nested_display_list_raster_matches_device_scale)
{
    auto context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
    if (!context) {
        warnln("No GPU backend available, skipping");
        return;
    }

    Gfx::IntSize size { 128, 128 };
    auto actual_surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, context);
    auto expected_surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, context);
    auto actual = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
    auto expected = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
    auto tree = Compositing::VisualContextTreeTestBuilder {}.finish();
    Compositor::DisplayListPlayerSkia player { context };
    Compositing::DisplayListResourceStorage storage;
    Compositor::DisplayListRasterCache cache;
    Compositing::FillRoundedRectRing ring {
        .rect = { 2, 2, 20, 20 },
        .corner_radii = { { 10, 10 }, { 10, 10 }, { 10, 10 }, { 10, 10 } },
        .top_width = 1,
        .right_width = 1,
        .bottom_width = 1,
        .left_width = 1,
        .color = Gfx::Color::Blue,
    };
    auto content = Compositing::VisualContextTreeTestBuilder::rounded_rect_ring_display_list(tree, {}, ring);
    auto id = storage.add_display_list(content, tree);
    auto nested = Compositing::VisualContextTreeTestBuilder::nested_display_list(tree, {}, {
                                                                                               .display_list_id = id,
                                                                                               .rect = { 0, 0, 24, 24 },
                                                                                               .list_size = { 24, 24 },
                                                                                           });

    // Warm the cache with a clipped paint, then compare the complete vector at several scales.
    for (auto scale : { Gfx::FloatSize { 1, 1 }, Gfx::FloatSize { 4, 4 }, Gfx::FloatSize { 2, 3 }, Gfx::FloatSize { 4, 4 } }) {
        for (bool clipped : { true, false }) {
            for (auto* surface : { actual_surface.ptr(), expected_surface.ptr() }) {
                surface->canvas().resetMatrix();
                surface->canvas().clear(SK_ColorTRANSPARENT);
                surface->canvas().save();
                if (clipped)
                    surface->canvas().clipRect(SkRect::MakeXYWH(13, 11, 40, 45));
                surface->canvas().scale(scale.width(), scale.height());
            }
            player.execute(*nested, tree, storage, cache, {}, actual_surface, nullptr, nullptr);
            player.execute(*content, tree, storage, cache, {}, expected_surface, nullptr, nullptr);
            actual_surface->canvas().restore();
            expected_surface->canvas().restore();
            if (clipped)
                continue;
            actual_surface->read_into_bitmap(*actual);
            expected_surface->read_into_bitmap(*expected);
            size_t different_pixels = 0;
            for (int y = 0; y < size.height(); ++y) {
                for (int x = 0; x < size.width(); ++x)
                    different_pixels += actual->get_pixel(x, y) != expected->get_pixel(x, y);
            }
            EXPECT_EQ(different_pixels, 0u);
        }
    }
}
