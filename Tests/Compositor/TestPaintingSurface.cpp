/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/SkiaUtils.h>
#include <LibGfx/Bitmap.h>
#include <LibTest/TestCase.h>

#if !defined(AK_OS_WINDOWS)
#    include <LibCore/Environment.h>
#    include <LibCore/System.h>
#    include <spawn.h>
#    include <sys/wait.h>
#    include <unistd.h>

extern char** environ;
#endif

#include <core/SkBlender.h>
#include <core/SkCanvas.h>
#include <core/SkPaint.h>
#include <core/SkPath.h>
#include <gpu/ganesh/GrDirectContext.h>

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

#if !defined(AK_OS_WINDOWS)
static constexpr auto lost_gpu_context_variable = "LADYBIRD_TEST_LOST_GPU_CONTEXT"sv;

// Runs only in the copy of this binary that the next test starts, which names the kind of submit in its environment.
TEST_CASE(lost_gpu_context_child)
{
    auto kind = Core::Environment::get(lost_gpu_context_variable);
    if (!kind.has_value())
        return;
    auto context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
    if (!context)
        _exit(77);
    auto surface = Gfx::PaintingSurface::create_with_size({ 4, 4 }, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, context);
    surface->canvas().clear(SK_ColorRED);
    // A GPU reset or a driver error leaves Skia's context abandoned.
    context->sk_context()->abandonContext();
    if (*kind == "synchronous"sv)
        context->flush_and_submit(*surface);
    else
        context->flush_and_submit_async(*surface, [] { });
    _exit(0);
}

TEST_CASE(a_submit_after_the_gpu_context_is_lost_exits_for_a_relaunch)
{
    // A fresh copy of this binary does the GPU work, since a forked child can't safely start using the GPU when the
    // parent already has.
    auto executable = MUST(Core::System::current_executable_path());
    for (auto kind : { "asynchronous"sv, "synchronous"sv }) {
        auto variable = ByteString::formatted("{}={}", lost_gpu_context_variable, kind);
        Vector<char*> environment;
        for (char** entry = environ; *entry; ++entry)
            environment.append(*entry);
        environment.append(const_cast<char*>(variable.characters()));
        environment.append(nullptr);
        char const* arguments[] = { executable.characters(), "lost_gpu_context_child", nullptr };
        pid_t child = 0;
        VERIFY(posix_spawn(&child, executable.characters(), nullptr, nullptr, const_cast<char* const*>(arguments), environment.data()) == 0);
        int status = 0;
        VERIFY(waitpid(child, &status, 0) == child);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
            warnln("No GPU backend available, skipping");
            return;
        }
        // Exiting, rather than crashing, lets the Browser relaunch the Compositor with no crash report.
        EXPECT(WIFEXITED(status));
        if (WIFEXITED(status))
            EXPECT_EQ(WEXITSTATUS(status), 1);
    }
}
#endif
