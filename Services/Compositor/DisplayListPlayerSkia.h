/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <Compositor/Forward.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/RustFFI.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

class GrDirectContext;
class SkColorFilter;
class SkImage;
class SkImageFilter;
class SkPaint;
class SkPath;
template<typename T>
class sk_sp;

namespace Compositor {

// The color filter force-dark runs classified images through: the same Oklab lightness inversion the solid colors
// take — as a Skia runtime effect. Exposed so a benchmark can time the shipped effect, rather than a copy of it.
sk_sp<SkColorFilter> force_dark_image_color_filter();

using PresentedScrollbar = Compositing::RustFFI::PresentedScrollbar;

// Paints with Skia whatever a Rust replay draws through the raster painter it is handed. It knows nothing of the
// visual context tree, scroll offsets or frames: every value it gets is resolved for what it draws.
class DisplayListPlayerSkia final {
public:
    AK_ALLOC_WITH_KMALLOC;

    DisplayListPlayerSkia();
    explicit DisplayListPlayerSkia(RefPtr<Gfx::SkiaBackendContext>);
    ~DisplayListPlayerSkia();

    // The painter paints into `surface`, with the resources of `resource_storage` and the caches of `raster_cache`,
    // until end_painting().
    Compositing::RustFFI::FfiRasterPainter begin_painting(Gfx::PaintingSurface&, Compositing::DisplayListResourceStorage const&, DisplayListRasterCache&, OwnedCanvasSurfaces const*);
    void end_painting();

    static void flush(Gfx::PaintingSurface&);
    static void flush_async(Gfx::PaintingSurface&, Function<void()>&&);

private:
    struct PainterCallbacks;

    Gfx::PaintingSurface& surface() const;
    Compositing::DisplayListResourceStorage const& resource_storage() const { return *m_resource_storage; }
    OwnedCanvasSurfaces const* canvas_surfaces() const { return m_canvas_surfaces; }
    DisplayListRasterCache& raster_cache() const { return *m_raster_cache; }

    ReadonlyBytes inline_data(Compositing::DisplayListDataSpan span) const
    {
        VERIFY(static_cast<size_t>(span.offset) + span.size <= m_current_command_payload.size());
        return m_current_command_payload.slice(span.offset, span.size);
    }
    template<typename T>
    ReadonlySpan<T> inline_objects(Compositing::DisplayListDataSpan span) const
    {
        static_assert(alignof(T) <= Compositing::display_list_payload_alignment);
        auto bytes = inline_data(span);
        VERIFY(bytes.size() % sizeof(T) == 0);
        VERIFY(reinterpret_cast<FlatPtr>(bytes.data()) % alignof(T) == 0);
        return { reinterpret_cast<T const*>(bytes.data()), bytes.size() / sizeof(T) };
    }

    void play_command_bytes(Compositing::DisplayListCommandType, u8 const* command, ReadonlyBytes payload);
#define DECLARE_PLAY_COMMAND(command_type) \
    void play_command(Compositing::command_type const&);
    ENUMERATE_DISPLAY_LIST_COMMANDS_PLAYED_AS_RECORDED(DECLARE_PLAY_COMMAND)
#undef DECLARE_PLAY_COMMAND

    void set_matrix(Gfx::FloatMatrix4x4 const&);
    Gfx::FloatMatrix4x4 canvas_matrix() const;
    bool would_be_fully_clipped_by_painter(Gfx::IntRect) const;
    void push_clip(Compositing::ReplayClip const&);
    void push_clip_path(Gfx::PathData const&, u64 path_identity, Gfx::WindingRule);
    void push_clip_path_bytes(ReadonlyBytes, Gfx::WindingRule, Compositing::ClipMode);
    void push_transform(Gfx::AffineTransform const&);
    void push_layer(Compositing::ReplayLayer const&);
    void push_mask(Compositing::ReplayMask const&, Compositing::ReplayLayer const* backdrop);
    void begin_mask_content(Compositing::ReplayMask const&);
    void pop_mask();
    void pop();
    void push_device_space_plane_clip(ReadonlySpan<Gfx::FloatVector3> polygon_vertices);
    void fill_rect(Compositing::RustFFI::ResolvedFillRect const&);
    void fill_rect_with_rounded_corners(Compositing::RustFFI::ResolvedFillRectWithRoundedCorners const&);
    void paint_scrollbar(PresentedScrollbar const&);
    void draw_composited_context(Compositing::RustFFI::CompositedContextDraw const&);
    void begin_isolated_group(Compositing::RustFFI::IsolatedGroup const&);
    void begin_isolated_group_mask(Gfx::MaskKind);
    void end_isolated_group_mask();
    void end_isolated_group();
    bool begin_tile_raster(Compositing::RustFFI::TileRaster const&);
    void end_tile_raster();
    Compositing::RustFFI::NestedDisplayListReplay begin_nested_display_list(Compositing::RustFFI::NestedDisplayListPlacement const&, bool requires_direct_replay);
    void end_nested_display_list();
    bool nested_display_list(u64 display_list_id, Compositing::RustFFI::FfiNestedDisplayList&) const;

    void clip_path(SkPath, Gfx::WindingRule, bool anti_aliased);

    SkPaint paint_style_to_skia_paint(Compositing::DisplayListPaintStyle const&, Gfx::FloatRect const& bounding_rect);
    sk_sp<SkImageFilter> layer_image_filter(Compositing::ReplayLayer const&);
    sk_sp<SkImageFilter> backdrop_image_filter(Compositing::ReplayLayer const&, bool limited_to_region);
    sk_sp<SkImageFilter> image_filter_from_bytes(ReadonlyBytes, Optional<Gfx::FloatRect> blur_edge = {});
    SkPath path_from_data(Compositing::DisplayListDataSpan) const;
    ReadonlySpan<Color> gradient_colors(Compositing::DisplayListGradientColorStops) const;
    ReadonlySpan<float> gradient_positions(Compositing::DisplayListGradientColorStops) const;

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;

    // The surface painted into last, which a tile or nested display list raster stacks on top of the target.
    Vector<NonnullRefPtr<Gfx::PaintingSurface>, 4> m_surfaces;
    Compositing::DisplayListResourceStorage const* m_resource_storage { nullptr };
    DisplayListRasterCache* m_raster_cache { nullptr };
    OwnedCanvasSurfaces const* m_canvas_surfaces { nullptr };
    ReadonlyBytes m_current_command_payload;

    // The tile and nested display list rasters the painter replays into, which hold Skia objects.
    struct RasterState;
    NonnullOwnPtr<RasterState> m_raster_state;
};

}
