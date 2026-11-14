/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/DisplayListPlayerSkia.h>
#include <Compositor/DisplayListRasterCache.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/RasterizeDisplayList.h>
#include <Compositor/SkiaBackendContext.h>
#include <LibGfx/Bitmap.h>

namespace Compositor {

// The largest frame WebContent rasterizes for an SVG image.
static constexpr int max_target_dimension = 16384;

ErrorOr<void> rasterize_display_list(Compositing::DisplayList const& display_list, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::DisplayListResourceTransaction&& resources, Gfx::Bitmap& target)
{
    if (target.format() != Gfx::BitmapFormat::BGRA8888 || target.alpha_type() != Gfx::AlphaType::Premultiplied)
        return Error::from_string_literal("Target bitmap is not BGRA8888 with premultiplied alpha");
    if (target.width() > max_target_dimension || target.height() > max_target_dimension)
        return Error::from_string_literal("Target bitmap is too large");
    if (!resources.font_ids_to_remove.is_empty() || !resources.image_frame_ids_to_remove.is_empty() || !resources.video_sink_ids_to_remove.is_empty() || !resources.display_list_ids_to_remove.is_empty())
        return Error::from_string_literal("Resource transaction removes resources");

    Compositing::DisplayListResourceStorage resource_storage;
    resource_storage.apply_transaction(move(resources));
    TRY(resource_storage.validate_for_replay(display_list, visual_context_tree));

    __builtin_memset(target.scanline_u8(0), 0, target.size_in_bytes());
    auto surface = Gfx::PaintingSurface::wrap_bitmap(target);

    // Raster on the CPU, so the pixels match a raster of the same list in any other process.
    DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    DisplayListRasterCache raster_cache;
    auto painter = display_list_player.begin_painting(*surface, resource_storage, raster_cache, nullptr);
    display_list.replay(visual_context_tree, painter, MonotonicTime::now().nanoseconds());
    display_list_player.end_painting();
    display_list_player.flush(*surface);
    return {};
}

}
