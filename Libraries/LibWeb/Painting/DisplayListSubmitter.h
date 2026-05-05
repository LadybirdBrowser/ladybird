/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Resource/Resource.h>
#include <LibGfx/Size.h>
#include <LibPaintServer/Compositor/DisplayListPayload.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Gfx {

class DecodedImageFrame;

}

namespace Web::Painting {

struct DisplayListStream {
    Function<ErrorOr<void>(size_t offset, ReadonlyBytes)> append_payload;
    Function<Optional<PaintServer::ReleaseToken>(PaintServer::FrameHeader const&, PaintServer::SyncToken render_wait_sync_token, Vector<Gfx::ResourceTransfer>&&)> finish;
    Function<Optional<PaintServer::ReleaseToken>(PaintServer::ImageID, Gfx::IntSize, Gfx::BitmapFormat, ReadonlyBytes, Vector<Gfx::ResourceTransfer>&&, Function<void(bool)>)> submit_canvas_draw_list;
    Function<void()> abort;
};

using DisplayListStreamSink = Function<Optional<DisplayListStream>()>;

class WEB_API DisplayListSubmitter {
    AK_MAKE_NONCOPYABLE(DisplayListSubmitter);
    AK_MAKE_NONMOVABLE(DisplayListSubmitter);

public:
    struct PageState;

    static void set_sink(u64 page_id, DisplayListStreamSink);
    static void clear_sink(u64 page_id);
    static void reset_all_resources();
    static void reset_resources_for_page(u64 page_id);
    static void invalidate_resource_for_page(u64 page_id, Gfx::ResourceID resource_id);
    static Optional<PaintServer::ResourceID> register_bitmap_resource(u64 page_id, Gfx::DecodedImageFrame const&, Optional<u64> stable_bitmap_id = {});
    static Optional<PaintServer::ResourceID> register_font_resource(u64 page_id, Gfx::Font const&);
    static Optional<PaintServer::ReleaseToken> submit_canvas_draw_list(u64 page_id, PaintServer::ImageID, Gfx::IntSize, Gfx::BitmapFormat, ReadonlyBytes, Function<void(bool)>);

    explicit DisplayListSubmitter(
        u64 sink_page_id,
        double device_pixels_per_css_pixel,
        Optional<Gfx::IntSize> frame_size,
        HashMap<FlatPtr, ScrollStateSnapshot> scroll_state_snapshots_by_source_context_namespace_id,
        PaintServer::FrameOutputType output_type = PaintServer::FrameOutputType::Presentation,
        PaintServer::OffscreenRenderTarget offscreen_target = {});
    ~DisplayListSubmitter();

    bool can_stream() const { return m_stream.has_value() && m_page_state != nullptr; }
    Optional<PaintServer::ReleaseToken> finalize(DisplayList const&);

private:
    struct EncodedVisualContextNode {
        PaintServer::VisualContextNodeKind kind { PaintServer::VisualContextNodeKind::Empty };
        u32 parent_index { 0 };
        bool has_empty_effective_clip { false };
        ByteBuffer setup_commands;
    };

    Optional<FlatPtr> source_context_namespace_id_for(VisualContextRef context, Optional<FlatPtr> current_source_context_namespace_id) const;
    ScrollStateSnapshot const* scroll_state_snapshot_for(Optional<FlatPtr> source_context_namespace_id) const;
    ErrorOr<u32> ensure_encoded_context_sequence(ReadonlySpan<VisualContextRef const> effective_context_nodes, Optional<FlatPtr> current_source_context_namespace_id);
    ErrorOr<u32> ensure_encoded_context_node(u32 encoded_parent, VisualContextRef context, Optional<FlatPtr> current_source_context_namespace_id);
    ErrorOr<ByteBuffer> encode_context_setup_commands(VisualContextRef context, Optional<FlatPtr> current_source_context_namespace_id);
    ErrorOr<void> append_display_list(DisplayList const& display_list, Optional<FlatPtr> source_context_namespace_id, ReadonlySpan<VisualContextRef const> inherited_context_nodes = {});
    ErrorOr<void> append_serialized_command(ReadonlyBytes command, ReadonlySpan<VisualContextRef const> effective_context_nodes, Optional<FlatPtr> current_source_context_namespace_id);
    ErrorOr<u32> append_payload_bytes(ReadonlyBytes bytes);

    u64 m_sink_page_id { 0 };
    f64 m_device_pixels_per_css_pixel { 1.0 };
    PageState* m_page_state { nullptr };
    PaintServer::FrameID m_frame_index { 0 };
    Optional<Gfx::IntSize> m_frame_size;
    PaintServer::FrameOutputType m_output_type { PaintServer::FrameOutputType::Presentation };
    PaintServer::OffscreenRenderTarget m_offscreen_target;
    HashMap<FlatPtr, ScrollStateSnapshot> m_root_scroll_state_snapshots;
    HashMap<FlatPtr, ScrollStateSnapshot> const* m_scroll_state_snapshots { nullptr };
    Optional<DisplayListStream> m_stream;
    size_t m_payload_bytes_written { 0 };
    size_t m_encoded_command_bytes_size { 0 };
    bool m_did_finalize { false };
    Vector<PaintServer::DisplayListCommandRecord> m_command_records;
    Vector<EncodedVisualContextNode> m_encoded_context_nodes;
    HashMap<u32, HashMap<VisualContextRef, u32>> m_encoded_context_index_by_parent;
    HashMap<VisualContextRef, FlatPtr> m_context_to_source_context_namespace_id;
};

}
