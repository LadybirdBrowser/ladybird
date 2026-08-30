/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
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
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

class WEB_API DisplayListPlayer {
public:
    virtual ~DisplayListPlayer() = default;

    void execute(DisplayList const&, AccumulatedVisualContextTree const&, DisplayListResourceStorage const&, ScrollStateSnapshot const&, RefPtr<Gfx::PaintingSurface>, CanvasSurfaceRegistry const* = nullptr);
    virtual void flush(Gfx::PaintingSurface&) = 0;

protected:
    Gfx::PaintingSurface& surface() const { return *m_surface; }
    DisplayList const& active_display_list() const { return *m_active_display_list; }
    AccumulatedVisualContextTree const& active_visual_context_tree() const { return *m_active_visual_context_tree; }
    DisplayListResourceStorage const& resource_storage() const { return *m_resource_storage; }
    CanvasSurfaceRegistry const* canvas_surface_registry() const { return m_canvas_surface_registry; }
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
    void execute_run_commands(DisplayListCommandRun const&, ScrollStateSnapshot const& scroll_state);
    Gfx::Filter const& layer_filter(ReplayLayer const&);
    void execute_display_list_into_surface(DisplayList const&, AccumulatedVisualContextTree const&, Gfx::PaintingSurface&);
    void execute_nested_display_list(DisplayList const&, AccumulatedVisualContextTree const&, ScrollStateSnapshot const&);

private:
#define DECLARE_PLAY_COMMAND(command_type, player_method) \
    virtual void play_command(command_type const&) = 0;
    ENUMERATE_DISPLAY_LIST_COMMANDS(DECLARE_PLAY_COMMAND)
#undef DECLARE_PLAY_COMMAND
    virtual void set_matrix(Gfx::FloatMatrix4x4 const&) = 0;
    virtual Gfx::FloatMatrix4x4 canvas_matrix() const = 0;
    virtual bool would_be_fully_clipped_by_painter(Gfx::IntRect) const = 0;

    virtual void push_clip(ReplayClip const&) = 0;
    virtual void push_clip_path(Gfx::Path const&, Gfx::WindingRule) = 0;
    virtual void push_layer(ReplayLayer const&) = 0;
    virtual void push_mask(ReplayMask const&) = 0;
    virtual void pop_mask(ReplayMask const&, Optional<DisplayListResourceId> mask_content) = 0;
    virtual void pop() = 0;
    virtual void push_device_space_plane_clip(Gfx::Path const&) = 0;

    DisplayList const* m_active_display_list { nullptr };
    AccumulatedVisualContextTree const* m_active_visual_context_tree { nullptr };
    DisplayListResourceStorage const* m_resource_storage { nullptr };
    CanvasSurfaceRegistry const* m_canvas_surface_registry { nullptr };
    RefPtr<Gfx::PaintingSurface> m_surface;
    ReadonlyBytes m_current_command_payload;

    struct CachedLayerFilter {
        ByteBuffer filter_bytes;
        Gfx::Filter filter;
    };
    HashMap<u64, HashMap<u32, CachedLayerFilter>> m_layer_filters_by_tree_version_and_frame;
    u64 m_layer_filter_cache_tree_version { 0 };
};

class DisplayList : public AtomicRefCounted<DisplayList> {
public:
    struct AsyncScrollingMetadata {
        Gfx::IntRect viewport_rect;
        u64 wheel_event_listener_state_generation { 0 };
        bool has_blocking_wheel_event_listeners { false };
        bool has_blocking_wheel_event_region_covering_viewport { false };
    };

    static NonnullRefPtr<DisplayList> create(AccumulatedVisualContextTree const& visual_context_tree)
    {
        return adopt_ref(*new DisplayList(visual_context_tree.version()));
    }

    static WEB_API NonnullRefPtr<DisplayList> create_from_command_bytes(AccumulatedVisualContextTree const&, ByteBuffer&& command_bytes, Vector<DisplayListCommandRun>&& command_runs);

    u64 compatible_visual_context_tree_version() const { return m_compatible_visual_context_tree_version; }
    u64 id() const { return m_id; }

    ReadonlyBytes command_bytes() const { return m_command_bytes.span(); }
    ReadonlySpan<DisplayListCommandRun> command_runs() const { return m_command_runs.span(); }
    ReadonlyBytes command_bytes_of_run(DisplayListCommandRun const& run) const { return command_bytes().slice(run.offset, run.size); }
    void set_surface_clear_color(Gfx::Color color) { m_surface_clear_color = color; }
    Optional<Gfx::Color> surface_clear_color() const { return m_surface_clear_color; }
    void set_async_scrolling_metadata(AsyncScrollingMetadata metadata) { m_async_scrolling_metadata = metadata; }
    Optional<AsyncScrollingMetadata> const& async_scrolling_metadata() const { return m_async_scrolling_metadata; }
    Optional<DisplayListResourceId> mask_display_list_id(FrameNodeIndex frame) const { return m_mask_display_lists.get(frame); }
    void set_mask_display_list_id(FrameNodeIndex frame, DisplayListResourceId display_list_id) { m_mask_display_lists.set(frame, display_list_id); }
    HashMap<FrameNodeIndex, DisplayListResourceId> const& mask_display_lists() const { return m_mask_display_lists; }

    static constexpr size_t command_alignment = 16;

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
    explicit DisplayList(u64 compatible_visual_context_tree_version);
    DisplayList(u64 compatible_visual_context_tree_version, u64 id, ByteBuffer&& command_bytes, Vector<DisplayListCommandRun>&& command_runs, Optional<Gfx::Color> surface_clear_color, Optional<AsyncScrollingMetadata>, HashMap<FrameNodeIndex, DisplayListResourceId>&& mask_display_lists);

    u64 m_compatible_visual_context_tree_version { 0 };
    u64 m_id { 0 };
    ByteBuffer m_command_bytes;
    Vector<DisplayListCommandRun> m_command_runs;
    Optional<Gfx::Color> m_surface_clear_color;
    Optional<AsyncScrollingMetadata> m_async_scrolling_metadata;
    HashMap<FrameNodeIndex, DisplayListResourceId> m_mask_display_lists;

    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);
    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);
};

// The run table the Rust builder records while it writes the tape, derived from the tape alone;
// tests build tapes by hand and debug builds check that both agree.
WEB_API Vector<DisplayListCommandRun> compute_display_list_command_runs(ReadonlyBytes command_bytes);
// Runs must start at offset zero, follow each other without gaps, stay aligned, end at the tape's
// end, and under DISPLAY_LIST_RUNS_DEBUG match the table recomputed from the tape.
WEB_API ErrorOr<void> validate_display_list_command_runs(ReadonlyBytes command_bytes, ReadonlySpan<DisplayListCommandRun>);

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::DisplayList::AsyncScrollingMetadata const&);
template<>
WEB_API ErrorOr<Web::Painting::DisplayList::AsyncScrollingMetadata> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::DisplayList const&);
template<>
WEB_API ErrorOr<void> encode(Encoder&, NonnullRefPtr<Web::Painting::DisplayList> const&);
template<>
WEB_API ErrorOr<NonnullRefPtr<Web::Painting::DisplayList>> decode(Decoder&);

}
