/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/Canvas2DCommandPlayer.h>
#include <Compositor/SkiaBackendContext.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibTest/TestCase.h>

TEST_CASE(drawing_strips_of_one_image_reuses_its_gpu_texture)
{
    auto context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
    if (!context) {
        warnln("No GPU backend available, skipping");
        return;
    }

    auto source = MUST(Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 512, 512 }));
    auto frame = MUST(Gfx::DecodedImageFrame::create(*source));
    Compositor::Canvas2DCommandPlayer::Resolvers resolvers;
    resolvers.image_frame = [&](u64) { return &frame; };
    auto player = MUST(Compositor::Canvas2DCommandPlayer::create(context, { 2, 1 }, frame.format(), frame.alpha_type(), {}, move(resolvers)));

    Compositing::Canvas2DDrawImageFrame draw {};
    draw.frame_id = Compositing::ImageFrameResourceId { frame.id() };
    draw.scaling_mode = Gfx::ScalingMode::BilinearMipmap;
    draw.compositing.global_alpha = 1;
    draw.compositing.compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::SourceOver;
    auto cache_bytes_after_drawing_strip = [&](float x) {
        draw.src_rect = { x, 0, 1, static_cast<float>(frame.height()) };
        draw.dst_rect = { x, 0, 1, 1 };
        player->play_command(draw.command_type, { &draw, sizeof(draw) });
        return context->resource_cache_bytes();
    };

    auto before = context->resource_cache_bytes();
    auto after_first_strip = cache_bytes_after_drawing_strip(0);
    EXPECT(after_first_strip > before);
    EXPECT(cache_bytes_after_drawing_strip(1) < after_first_strip + frame.size_in_bytes());
}
