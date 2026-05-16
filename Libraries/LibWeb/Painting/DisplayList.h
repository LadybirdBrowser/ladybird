/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <LibGfx/Color.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Forward.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ExternalContentSource.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/VideoFrameSource.h>

namespace Web::Painting {

class DisplayListCommandSequence {
public:
    static constexpr size_t command_alignment = 16;

    ReadonlyBytes command_bytes() const { return m_command_bytes.span(); }

    template<typename SpanType, typename Callback>
    static void for_each_command_header(SpanType command_bytes, Callback callback)
    {
        static_assert(IsSame<SpanType, Bytes> || IsSame<SpanType, ReadonlyBytes>);
        for (size_t offset = 0; offset < command_bytes.size();) {
            VERIFY(offset + sizeof(DisplayListCommandHeader) <= command_bytes.size());
            auto header = read_display_list_object<DisplayListCommandHeader>(command_bytes.slice(offset));
            offset += sizeof(header);
            VERIFY(offset + header.payload_size <= command_bytes.size());
            auto payload = SpanType { command_bytes.data() + offset, header.payload_size };
            offset += header.payload_size;
            callback(header, payload);
        }
    }

    template<typename Callback>
    void for_each_command_header(Callback callback) const
    {
        for_each_command_header(command_bytes(), move(callback));
    }

private:
    friend class DisplayList;
    DisplayListCommandSequence();

    DisplayListResourceStorage m_resource_storage;
    ByteBuffer m_command_bytes;
};

class DisplayListPlayer {
public:
    virtual ~DisplayListPlayer() = default;

    void execute(DisplayList const&, DisplayListResourceStorage const&, ScrollStateSnapshot const&, RefPtr<Gfx::PaintingSurface>);

protected:
    Gfx::PaintingSurface& surface() const { return *m_surface; }
    DisplayList const& active_display_list() const { return *m_active_display_list; }
    DisplayListResourceStorage const& resource_storage() const { return *m_resource_storage; }
    ReadonlyBytes inline_data(DisplayListDataSpan span) const
    {
        VERIFY(static_cast<size_t>(span.offset) + span.size <= m_current_command_payload.size());
        return m_current_command_payload.slice(span.offset, span.size);
    }
    template<typename T>
    ReadonlySpan<T> inline_objects(DisplayListDataSpan span) const
    {
        auto bytes = inline_data(span);
        VERIFY(bytes.size() % sizeof(T) == 0);
        VERIFY(reinterpret_cast<FlatPtr>(bytes.data()) % alignof(T) == 0);
        return { reinterpret_cast<T const*>(bytes.data()), bytes.size() / sizeof(T) };
    }
    void execute_impl(DisplayList const&, ScrollStateSnapshot const& scroll_state);
    void execute_impl(DisplayList const&, ScrollStateSnapshot const& scroll_state, ReadonlyBytes command_bytes);
    void execute_display_list_into_surface(DisplayList const&, Gfx::PaintingSurface&);
    void execute_nested_display_list(DisplayList const&, ScrollStateSnapshot const&, ReadonlyBytes command_bytes);

private:
    virtual void flush() = 0;
    virtual void draw_glyph_run(DrawGlyphRun const&) = 0;
    virtual void fill_rect(FillRect const&) = 0;
    virtual void draw_scaled_decoded_image_frame(DrawScaledDecodedImageFrame const&) = 0;
    virtual void draw_repeated_decoded_image_frame(DrawRepeatedDecodedImageFrame const&) = 0;
    virtual void draw_external_content(DrawExternalContent const&) = 0;
    virtual void draw_video_frame_source(DrawVideoFrameSource const&) = 0;
    virtual void save(Save const&) = 0;
    virtual void save_layer(SaveLayer const&) = 0;
    virtual void restore(Restore const&) = 0;
    virtual void translate(Translate const&) = 0;
    virtual void add_clip_rect(AddClipRect const&) = 0;
    virtual void paint_linear_gradient(PaintLinearGradient const&) = 0;
    virtual void paint_radial_gradient(PaintRadialGradient const&) = 0;
    virtual void paint_conic_gradient(PaintConicGradient const&) = 0;
    virtual void paint_outer_box_shadow(PaintOuterBoxShadow const&) = 0;
    virtual void paint_inner_box_shadow(PaintInnerBoxShadow const&) = 0;
    virtual void paint_text_shadow(PaintTextShadow const&) = 0;
    virtual void fill_rect_with_rounded_corners(FillRectWithRoundedCorners const&) = 0;
    virtual void fill_path(FillPath const&) = 0;
    virtual void stroke_path(StrokePath const&) = 0;
    virtual void draw_ellipse(DrawEllipse const&) = 0;
    virtual void fill_ellipse(FillEllipse const&) = 0;
    virtual void draw_line(DrawLine const&) = 0;
    virtual void apply_backdrop_filter(ApplyBackdropFilter const&) = 0;
    virtual void draw_rect(DrawRect const&) = 0;
    virtual void add_rounded_rect_clip(AddRoundedRectClip const&) = 0;
    virtual void paint_nested_display_list(PaintNestedDisplayList const&) = 0;
    virtual void compositor_scroll_node(CompositorScrollNode const&) = 0;
    virtual void compositor_sticky_area(CompositorStickyArea const&) = 0;
    virtual void compositor_wheel_hit_test_target(CompositorWheelHitTestTarget const&) = 0;
    virtual void compositor_main_thread_wheel_event_region(CompositorMainThreadWheelEventRegion const&) = 0;
    virtual void compositor_viewport_scrollbar(CompositorViewportScrollbar const&) = 0;
    virtual void compositor_blocking_wheel_event_region(CompositorBlockingWheelEventRegion const&) = 0;
    virtual void paint_scrollbar(PaintScrollBar const&) = 0;
    virtual void apply_effects(ApplyEffects const&, Gfx::Filter const* = nullptr) = 0;
    virtual void apply_transform(Gfx::FloatPoint origin, Gfx::FloatMatrix4x4 const&) = 0;
    virtual bool would_be_fully_clipped_by_painter(Gfx::IntRect) const = 0;

    virtual void add_clip_path(Gfx::Path const&) = 0;

    DisplayList const* m_active_display_list { nullptr };
    DisplayListResourceStorage const* m_resource_storage { nullptr };
    RefPtr<Gfx::PaintingSurface> m_surface;
    ReadonlyBytes m_current_command_payload;
};

class DisplayList : public AtomicRefCounted<DisplayList> {
public:
    struct AsyncScrollingMetadata {
        Gfx::IntRect viewport_rect;
        u64 wheel_event_listener_state_generation { 0 };
        bool has_blocking_wheel_event_listeners { false };
        bool has_blocking_wheel_event_region_covering_viewport { false };
    };

    static NonnullRefPtr<DisplayList> create(NonnullRefPtr<AccumulatedVisualContextTree const> visual_context_tree)
    {
        return adopt_ref(*new DisplayList(move(visual_context_tree)));
    }

    template<DisplayListCommand Command>
    bool append(Command const& command, VisualContextIndex context_index, ReadonlyBytes inline_data = {})
    {
        return append_bytes(
            Command::command_type,
            display_list_object_bytes(command),
            inline_data,
            context_index,
            command_bounding_rectangle(command),
            command_is_clip(command));
    }

    AccumulatedVisualContextTree const& visual_context_tree() const { return *m_visual_context_tree; }
    u64 id() const { return m_id; }

    ReadonlyBytes command_bytes() const { return m_command_bytes.span(); }
    void set_async_scrolling_metadata(AsyncScrollingMetadata metadata) { m_async_scrolling_metadata = metadata; }
    Optional<AsyncScrollingMetadata> const& async_scrolling_metadata() const { return m_async_scrolling_metadata; }

    template<typename Callback>
    static void for_each_command_header(ReadonlyBytes command_bytes, Callback callback)
    {
        DisplayListCommandSequence::for_each_command_header(command_bytes, move(callback));
    }

    template<typename Callback>
    void for_each_command_header(Callback callback) const
    {
        DisplayListCommandSequence::for_each_command_header(command_bytes(), move(callback));
    }

    void append_command_sequence(DisplayListCommandSequence const&, VisualContextIndex, DisplayListResourceStorage&);
    DisplayListCommandSequence copy_command_sequence_from(size_t command_start_offset, DisplayListResourceStorage const&) const;
    size_t command_byte_size() const { return m_command_bytes.size(); }

private:
    explicit DisplayList(NonnullRefPtr<AccumulatedVisualContextTree const> visual_context_tree);

    static Optional<Gfx::IntRect> command_bounding_rectangle(auto const& command)
    {
        if constexpr (requires { command.bounding_rect(); })
            return command.bounding_rect();
        else
            return {};
    }

    static bool command_is_clip(auto const& command)
    {
        if constexpr (requires { command.is_clip(); })
            return command.is_clip();
        else
            return false;
    }

    bool append_bytes(
        DisplayListCommandType,
        ReadonlyBytes payload,
        ReadonlyBytes inline_data,
        VisualContextIndex context_index,
        Optional<Gfx::IntRect> bounding_rect,
        bool is_clip);

    NonnullRefPtr<AccumulatedVisualContextTree const> const m_visual_context_tree;
    u64 m_id { 0 };
    ByteBuffer m_command_bytes;
    Optional<AsyncScrollingMetadata> m_async_scrolling_metadata;
};

}
