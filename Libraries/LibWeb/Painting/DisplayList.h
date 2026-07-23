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
#include <LibWeb/Painting/DisplayListCommandRange.h>
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
    void execute_impl(DisplayList const&, ScrollStateSnapshot const& scroll_state, ReadonlyBytes command_bytes);
    void execute_display_list_into_surface(DisplayList const&, AccumulatedVisualContextTree const&, Gfx::PaintingSurface&);
    void execute_nested_display_list(DisplayList const&, AccumulatedVisualContextTree const&, ScrollStateSnapshot const&, ReadonlyBytes command_bytes);

private:
#define DECLARE_PLAY_COMMAND(command_type, player_method) \
    virtual void play_command(command_type const&) = 0;
    ENUMERATE_DISPLAY_LIST_COMMANDS(DECLARE_PLAY_COMMAND)
#undef DECLARE_PLAY_COMMAND
    virtual void play_command(ApplyEffects const&, Gfx::Filter const*) = 0;
    virtual void set_matrix(Gfx::FloatMatrix4x4 const&) = 0;
    virtual Gfx::FloatMatrix4x4 canvas_matrix() const = 0;
    virtual bool would_be_fully_clipped_by_painter(Gfx::IntRect) const = 0;

    virtual void add_clip_path(Gfx::Path const&, Gfx::WindingRule) = 0;

    DisplayList const* m_active_display_list { nullptr };
    AccumulatedVisualContextTree const* m_active_visual_context_tree { nullptr };
    DisplayListResourceStorage const* m_resource_storage { nullptr };
    CanvasSurfaceRegistry const* m_canvas_surface_registry { nullptr };
    RefPtr<Gfx::PaintingSurface> m_surface;
    ReadonlyBytes m_current_command_payload;

    // Scratch for the per-replay transform palette, retained so steady-state replays reuse warm
    // capacity; execute_impl moves it out for the duration of a replay, letting re-entrant nested
    // replays build their own palettes without clobbering the outer one.
    struct ReplayPaletteStorage {
        Vector<Gfx::FloatMatrix4x4> to_root_matrices;
        Vector<VisualContextIndex> nearest_spatial_nodes;
        Vector<VisualContextIndex> nearest_frame_nodes;
    };
    ReplayPaletteStorage m_replay_palette_storage;
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

    template<DisplayListCommand Command>
    bool append(Command const& command, AccumulatedVisualContextTree const& visual_context_tree, VisualContextIndex context_index, bool context_geometry_only, ReadonlyBytes inline_data = {})
    {
        return append_bytes(
            Command::command_type,
            display_list_object_bytes(command),
            inline_data,
            visual_context_tree,
            context_index,
            context_geometry_only,
            command_bounding_rectangle(command),
            command_is_clip(command));
    }

    u64 compatible_visual_context_tree_version() const { return m_compatible_visual_context_tree_version; }
    u64 id() const { return m_id; }

    ReadonlyBytes command_bytes() const { return m_command_bytes.span(); }
    void set_surface_clear_color(Gfx::Color color) { m_surface_clear_color = color; }
    Optional<Gfx::Color> surface_clear_color() const { return m_surface_clear_color; }
    void set_async_scrolling_metadata(AsyncScrollingMetadata metadata) { m_async_scrolling_metadata = metadata; }
    Optional<AsyncScrollingMetadata> const& async_scrolling_metadata() const { return m_async_scrolling_metadata; }
    Optional<DisplayListResourceId> mask_display_list_id(VisualContextIndex context_index) const { return m_mask_display_lists.get(context_index); }
    void set_mask_display_list_id(VisualContextIndex context_index, DisplayListResourceId display_list_id) { m_mask_display_lists.set(context_index, display_list_id); }
    HashMap<VisualContextIndex, DisplayListResourceId> const& mask_display_lists() const { return m_mask_display_lists; }

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

    u32 append_command_range_from(DisplayList const& source_display_list, DisplayListCommandRange, AccumulatedVisualContextTree const&, VisualContextIndex recorded_context_index, VisualContextIndex current_context_index);
    size_t command_byte_size() const { return m_command_bytes.size(); }

private:
    explicit DisplayList(u64 compatible_visual_context_tree_version);
    DisplayList(u64 compatible_visual_context_tree_version, u64 id, ByteBuffer&& command_bytes, Optional<Gfx::Color> surface_clear_color, Optional<AsyncScrollingMetadata>, HashMap<VisualContextIndex, DisplayListResourceId>&& mask_display_lists);

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
        AccumulatedVisualContextTree const&,
        VisualContextIndex context_index,
        bool context_geometry_only,
        Optional<Gfx::IntRect> bounding_rect,
        bool is_clip);

    u64 m_compatible_visual_context_tree_version { 0 };
    u64 m_id { 0 };
    ByteBuffer m_command_bytes;
    Optional<Gfx::Color> m_surface_clear_color;
    Optional<AsyncScrollingMetadata> m_async_scrolling_metadata;
    HashMap<VisualContextIndex, DisplayListResourceId> m_mask_display_lists;

    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);
    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);
};

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
