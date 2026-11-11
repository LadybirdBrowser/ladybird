/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <Compositor/Forward.h>
#include <Compositor/FrameInputs.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/Forward.h>

namespace Compositor {

class DisplayListPlayer {
public:
    virtual ~DisplayListPlayer() = default;

    // The frame inputs say where the frame presents what scrolls. Without them, nothing is scrolled.
    void execute(Compositing::DisplayList const&, Compositing::AccumulatedVisualContextTree const&, Compositing::DisplayListResourceStorage const&, FrameInputs const*, RefPtr<Gfx::PaintingSurface>, OwnedCanvasSurfaces const* = nullptr);
    virtual void flush(Gfx::PaintingSurface&) = 0;

protected:
    Gfx::PaintingSurface& surface() const { return *m_surface; }
    Compositing::DisplayList const& active_display_list() const { return *m_active_display_list; }
    Compositing::AccumulatedVisualContextTree const& active_visual_context_tree() const { return *m_active_visual_context_tree; }
    Compositing::DisplayListResourceStorage const& resource_storage() const { return *m_resource_storage; }
    OwnedCanvasSurfaces const* canvas_surfaces() const { return m_canvas_surfaces; }
    void execute_impl(Compositing::DisplayList const&, FrameInputs const*);
    void execute_display_list_into_surface(Compositing::DisplayList const&, Compositing::AccumulatedVisualContextTree const&, Gfx::PaintingSurface&);
    void execute_nested_display_list(Compositing::DisplayList const&, Compositing::AccumulatedVisualContextTree const&);
    // Draws into another surface until the matching end_drawing_into_surface(), which returns that surface.
    void begin_drawing_into_surface(NonnullRefPtr<Gfx::PaintingSurface>);
    NonnullRefPtr<Gfx::PaintingSurface> end_drawing_into_surface();

private:
    struct ReplayCallbacks;
    void play_command(Compositing::DisplayListCommandType, void const* command, Compositing::PlayedPayload const&);

#define DECLARE_PLAY_COMMAND(command_type) \
    virtual void play_command(Compositing::command_type const&, Compositing::PlayedPayload const&) = 0;
    ENUMERATE_PLAYED_DISPLAY_LIST_COMMANDS(DECLARE_PLAY_COMMAND)
#undef DECLARE_PLAY_COMMAND
    virtual void set_matrix(Gfx::FloatMatrix4x4 const&) = 0;
    virtual Gfx::FloatMatrix4x4 canvas_matrix() const = 0;
    virtual bool would_be_fully_clipped_by_painter(Gfx::IntRect) const = 0;

    virtual void push_clip(Compositing::ReplayClip const&) = 0;
    // A path stays the same for as long as it has its identity.
    virtual void push_clip_path(Gfx::PathData const&, u64 path_identity, Gfx::WindingRule) = 0;
    // The path is not kept beyond the clip.
    virtual void push_uncached_clip_path(Gfx::PathData const&, Gfx::WindingRule) = 0;
    virtual void push_transform(Gfx::AffineTransform const&) = 0;
    virtual void push_layer(Compositing::ReplayLayer const&) = 0;
    virtual void push_mask(Compositing::ReplayMask const&, Compositing::ReplayLayer const* backdrop) = 0;
    virtual void begin_mask_content(Compositing::ReplayMask const&) = 0;
    virtual void end_mask() = 0;
    virtual void begin_isolated_group(Compositing::DrawIsolatedGroup const&, ReadonlyBytes filter) = 0;
    virtual void begin_isolated_group_mask(Gfx::MaskKind) = 0;
    virtual void end_isolated_group() = 0;
    virtual bool use_cached_tile_raster(u64 key, Gfx::IntSize) = 0;
    virtual bool begin_tile_raster(Gfx::IntSize) = 0;
    virtual void end_tile_raster(Optional<u64> cache_key) = 0;
    virtual void pop() = 0;
    virtual void push_device_space_plane_clip(ReadonlySpan<Gfx::FloatVector3> polygon_vertices) = 0;

    Compositing::DisplayList const* m_active_display_list { nullptr };
    Compositing::AccumulatedVisualContextTree const* m_active_visual_context_tree { nullptr };
    Compositing::DisplayListResourceStorage const* m_resource_storage { nullptr };
    OwnedCanvasSurfaces const* m_canvas_surfaces { nullptr };
    RefPtr<Gfx::PaintingSurface> m_surface;
    Vector<NonnullRefPtr<Gfx::PaintingSurface>> m_surfaces_drawn_into_before;
};

}
