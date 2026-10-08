/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibGfx/Forward.h>

namespace Compositor {

class DisplayListPlayer {
public:
    virtual ~DisplayListPlayer() = default;

    void execute(Compositing::DisplayList const&, Compositing::AccumulatedVisualContextTree const&, Compositing::DisplayListResourceStorage const&, Compositing::ScrollStateSnapshot const&, RefPtr<Gfx::PaintingSurface>, Compositing::CanvasSurfaceRegistry const* = nullptr);
    virtual void flush(Gfx::PaintingSurface&) = 0;

protected:
    Gfx::PaintingSurface& surface() const { return *m_surface; }
    Compositing::DisplayList const& active_display_list() const { return *m_active_display_list; }
    Compositing::AccumulatedVisualContextTree const& active_visual_context_tree() const { return *m_active_visual_context_tree; }
    Compositing::DisplayListResourceStorage const& resource_storage() const { return *m_resource_storage; }
    Compositing::CanvasSurfaceRegistry const* canvas_surface_registry() const { return m_canvas_surface_registry; }
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
    void execute_impl(Compositing::DisplayList const&, Compositing::ScrollStateSnapshot const& scroll_state);
    void execute_command_bytes(ReadonlyBytes, Compositing::ScrollStateSnapshot const& scroll_state);
    Compositing::ScrollStateSnapshot const& active_scroll_state() const { return *m_active_scroll_state; }
    void execute_display_list_into_surface(Compositing::DisplayList const&, Compositing::AccumulatedVisualContextTree const&, Gfx::PaintingSurface&);
    void execute_command_bytes_into_surface(ReadonlyBytes, Gfx::PaintingSurface&);
    void declare_mask_content(Compositing::EffectNodeIndex, ReadonlyBytes content);
    Optional<ReadonlyBytes> declared_mask_content(Compositing::EffectNodeIndex) const;
    void execute_nested_display_list(Compositing::DisplayList const&, Compositing::AccumulatedVisualContextTree const&, Compositing::ScrollStateSnapshot const&);

private:
    struct ReplayCallbacks;
    void play_command_bytes(Compositing::DisplayListCommandType, u8 const* command, ReadonlyBytes payload);

#define DECLARE_PLAY_COMMAND(command_type) \
    virtual void play_command(Compositing::command_type const&) = 0;
    ENUMERATE_DISPLAY_LIST_COMMANDS(DECLARE_PLAY_COMMAND)
#undef DECLARE_PLAY_COMMAND
    virtual void set_matrix(Gfx::FloatMatrix4x4 const&) = 0;
    virtual Gfx::FloatMatrix4x4 canvas_matrix() const = 0;
    virtual bool would_be_fully_clipped_by_painter(Gfx::IntRect) const = 0;

    virtual void push_clip(Compositing::ReplayClip const&) = 0;
    virtual void push_clip_path(Gfx::Path const&, Gfx::WindingRule) = 0;
    virtual void push_transform(Gfx::AffineTransform const&) = 0;
    virtual void push_layer(Compositing::ReplayLayer const&) = 0;
    virtual void push_mask(Compositing::ReplayMask const&) = 0;
    virtual void pop_mask(Compositing::ReplayMask const&, Compositing::EffectNodeIndex) = 0;
    virtual void pop() = 0;
    virtual void push_device_space_plane_clip(Gfx::Path const&) = 0;

    Compositing::DisplayList const* m_active_display_list { nullptr };
    Compositing::AccumulatedVisualContextTree const* m_active_visual_context_tree { nullptr };
    Compositing::DisplayListResourceStorage const* m_resource_storage { nullptr };
    Compositing::CanvasSurfaceRegistry const* m_canvas_surface_registry { nullptr };
    RefPtr<Gfx::PaintingSurface> m_surface;
    ReadonlyBytes m_current_command_payload;
    Compositing::ScrollStateSnapshot const* m_active_scroll_state { nullptr };
    HashMap<u32, ReadonlyBytes> m_declared_mask_contents;
};

}
