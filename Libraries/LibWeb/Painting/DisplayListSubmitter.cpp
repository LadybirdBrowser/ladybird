/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <AK/Time.h>
#include <AK/TypedTransfer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Resource/BitmapResource.h>
#include <LibGfx/Resource/FontResource.h>
#include <LibPaintServer/Compositor/DisplayListPayload.h>
#include <LibPaintServer/Compositor/DrawCommands.h>
#include <LibPaintServer/Compositor/DrawList.h>
#include <LibPaintServer/Debug.h>
#include <LibWeb/Painting/DisplayListSubmitter.h>

namespace Web::Painting {

struct DisplayListSubmitter::PageState {
    Optional<DisplayListStreamSink> sink;
    Gfx::BitmapRegistry bitmap_registry;
    HashMap<u64, PaintServer::ResourceID> bitmap_uid_to_resource_id;
    PaintServer::FrameID frame_index { 0 };
    NonnullOwnPtr<Gfx::FontResourceRegistry> font_registry { make<Gfx::FontResourceRegistry>() };
    PaintServer::GPUEpoch payload_generation { 0 };
    PaintServer::SyncToken pending_render_wait_sync_token { 0 };

    void reset()
    {
        bitmap_registry.clear();
        bitmap_uid_to_resource_id.clear();
        frame_index = 0;
        payload_generation = 0;
        pending_render_wait_sync_token = 0;
        font_registry->clear();
    }
};

static HashMap<u64, NonnullOwnPtr<DisplayListSubmitter::PageState>>& page_states();
static DisplayListSubmitter::PageState& ensure_page_state(u64 page_id);
static DisplayListSubmitter::PageState* page_state(u64 page_id);
static Vector<Gfx::ResourceTransfer> take_pending_resource_submissions(DisplayListSubmitter::PageState& page_state);

static PaintServer::ResourceID allocate_bitmap_resource_id()
{
    static u32 s_next_bitmap_resource_id_seed = 1;
    return Gfx::ResourceID::make(Gfx::WireResourceType::Bitmap, s_next_bitmap_resource_id_seed++);
}

static Optional<PaintCommandEncodingContext> page_encoding_context(u64 page_id, double device_pixels_per_css_pixel)
{
    auto* state = page_state(page_id);
    if (!state)
        return {};
    return PaintCommandEncodingContext {
        .ensure_bitmap_resource = [state](Gfx::DecodedImageFrame const& frame, Optional<u64> stable_bitmap_id) -> Optional<PaintServer::ResourceID> {
            u64 const bitmap_uid = stable_bitmap_id.value_or(static_cast<u64>(reinterpret_cast<FlatPtr>(frame.bitmap_ref().ptr())));
            auto it = state->bitmap_uid_to_resource_id.find(bitmap_uid);
            PaintServer::ResourceID stable_resource_id { 0 };
            if (it == state->bitmap_uid_to_resource_id.end()) {
                stable_resource_id = allocate_bitmap_resource_id();
                state->bitmap_uid_to_resource_id.set(bitmap_uid, stable_resource_id);
            } else {
                stable_resource_id = it->value;
            }

            return state->bitmap_registry.register_bitmap(stable_resource_id, frame);
        },
        .ensure_font_resource = [state](Gfx::Font const& font) -> PaintServer::ResourceID {
            return state->font_registry->ensure_font_resource(font);
        },
        .device_pixels_per_css_pixel = device_pixels_per_css_pixel,
    };
}

static Gfx::IntRect int_rect_from_float_rect(Gfx::FloatRect const& rect)
{
    return {
        static_cast<int>(rect.x()),
        static_cast<int>(rect.y()),
        static_cast<int>(rect.width()),
        static_cast<int>(rect.height())
    };
}

DisplayListSubmitter::DisplayListSubmitter(
    u64 sink_page_id,
    double device_pixels_per_css_pixel,
    Optional<Gfx::IntSize> frame_size,
    HashMap<FlatPtr, ScrollStateSnapshot> scroll_state_snapshots_by_source_context_namespace_id,
    PaintServer::FrameOutputType output_type,
    PaintServer::OffscreenRenderTarget offscreen_target)
    : m_sink_page_id(sink_page_id)
    , m_device_pixels_per_css_pixel(device_pixels_per_css_pixel)
    , m_frame_size(frame_size)
    , m_output_type(output_type)
    , m_offscreen_target(offscreen_target)
    , m_root_scroll_state_snapshots(move(scroll_state_snapshots_by_source_context_namespace_id))
    , m_scroll_state_snapshots(&m_root_scroll_state_snapshots)
{
    m_encoded_context_nodes.append({});

    m_page_state = page_state(sink_page_id);
    if (!m_page_state) {
        if (PaintServer::is_logging_enabled(PaintServer::LOG_INGRESS))
            dbgln("DisplayListSubmitter: stream disabled reason=no_page_state page_id={}", sink_page_id);
        return;
    }

    m_frame_index = ++m_page_state->frame_index;
    if (!m_page_state->sink.has_value()) {
        if (PaintServer::is_logging_enabled(PaintServer::LOG_INGRESS))
            dbgln("DisplayListSubmitter: stream disabled reason=no_sink page_id={} frame_id={}", sink_page_id, m_frame_index);
        return;
    }

    m_stream = m_page_state->sink.value()();
    if (!m_stream.has_value()) {
        if (PaintServer::is_logging_enabled(PaintServer::LOG_INGRESS))
            dbgln("DisplayListSubmitter: stream disabled reason=create_submission_failed page_id={} frame_id={}", sink_page_id, m_frame_index);
        return;
    }

    if (PaintServer::is_logging_enabled(PaintServer::LOG_INGRESS))
        dbgln("DisplayListSubmitter: begin frame page_id={} frame_id={} device_pixels_per_css_pixel={} frame_size={}", sink_page_id, m_frame_index, device_pixels_per_css_pixel, frame_size.value_or(Gfx::IntSize { 0, 0 }));
}

DisplayListSubmitter::~DisplayListSubmitter()
{
    if (m_stream.has_value())
        m_stream->abort();
}

Optional<PaintServer::ReleaseToken> DisplayListSubmitter::finalize(DisplayList const& display_list)
{
    if (m_did_finalize)
        return {};
    m_did_finalize = true;

    Optional<Core::ElapsedTimer> encode_timer;
    if (PaintServer::is_logging_enabled(PaintServer::LOG_TIMING))
        encode_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    if (!can_stream()) {
        if (PaintServer::is_logging_enabled(PaintServer::LOG_INGRESS))
            dbgln("DisplayListSubmitter: finalize skipped reason=no_stream page_id={} frame_id={}", m_sink_page_id, m_frame_index);
        return {};
    }

    auto finalize_result = [&]() -> ErrorOr<PaintServer::ReleaseToken> {
        TRY(append_display_list(display_list, display_list.source_context_namespace_id()));

        u32 const command_records_offset = TRY(append_payload_bytes(ReadonlyBytes { m_command_records.data(), m_command_records.size() * sizeof(PaintServer::DisplayListCommandRecord) }));

        Vector<PaintServer::VisualContextNodeRecord> node_records;
        TRY(node_records.try_resize(m_encoded_context_nodes.size()));
        for (size_t index = 1; index < m_encoded_context_nodes.size(); ++index) {
            auto const& node = m_encoded_context_nodes[index];
            auto& record = node_records[index];
            record.kind = to_underlying(node.kind);
            record.flags = node.has_empty_effective_clip ? PaintServer::HasEmptyEffectiveClip : PaintServer::None;
            record.parent_index = node.parent_index;
            if (!node.setup_commands.is_empty()) {
                record.data_offset = TRY(append_payload_bytes(node.setup_commands.bytes()));
                record.data_size = static_cast<u32>(node.setup_commands.size());
            }
        }

        u32 const visual_context_nodes_offset = TRY(append_payload_bytes(ReadonlyBytes { node_records.data(), node_records.size() * sizeof(PaintServer::VisualContextNodeRecord) }));

        Checked<size_t> total_payload_bytes = m_payload_bytes_written;
        total_payload_bytes += sizeof(PaintServer::DisplayListPayloadFooter);
        if (total_payload_bytes.has_overflow() || total_payload_bytes.value() > NumericLimits<u32>::max())
            return Error::from_string_literal("DisplayListSubmitter payload is too large");

        PaintServer::DisplayListPayloadFooter footer {
            .total_payload_bytes = static_cast<u32>(total_payload_bytes.value()),
            .command_stream_offset = 0,
            .command_stream_size = static_cast<u32>(m_encoded_command_bytes_size),
            .command_records_offset = command_records_offset,
            .command_record_count = static_cast<u32>(m_command_records.size()),
            .visual_context_nodes_offset = visual_context_nodes_offset,
            .visual_context_node_count = static_cast<u32>(m_encoded_context_nodes.size()),
            .scroll_frames_offset = 0,
            .scroll_frame_count = 0,
        };
        TRY(append_payload_bytes(ReadonlyBytes { &footer, sizeof(footer) }));

        ++m_page_state->payload_generation;
        PaintServer::FrameHeader header;
        header.output_type = m_output_type;
        header.offscreen_target = m_offscreen_target;
        if (m_frame_size.has_value())
            header.viewport_size = { static_cast<f32>(AK::max(1, m_frame_size->width())), static_cast<f32>(AK::max(1, m_frame_size->height())) };
        else
            header.viewport_size = { 1.0f, 1.0f };
        header.payload_generation = m_page_state->payload_generation;
        header.device_pixels_per_css_pixel = static_cast<f32>(m_device_pixels_per_css_pixel);
        header.webcontent_submission_timestamp_ms = static_cast<u64>(MonotonicTime::now().milliseconds());
        header.payload_length = static_cast<u32>(total_payload_bytes.value());

        PaintServer::SyncToken const render_wait_sync_token = exchange(m_page_state->pending_render_wait_sync_token, 0);
        Vector<Gfx::ResourceTransfer> resource_submissions = take_pending_resource_submissions(*m_page_state);
        auto release_token = m_stream->finish(header, render_wait_sync_token, move(resource_submissions));
        if (!release_token.has_value())
            return Error::from_string_literal("DisplayListSubmitter failed to finish streamed submission");

        if (PaintServer::is_logging_enabled(PaintServer::LOG_INGRESS)) {
            dbgln("DisplayListSubmitter: finish frame page_id={} frame_id={} payload_generation={} payload_bytes={} commands={} visual_context_nodes={} encoded_command_bytes={}",
                m_sink_page_id,
                m_frame_index,
                header.payload_generation,
                header.payload_length,
                m_command_records.size(),
                m_encoded_context_nodes.size(),
                m_encoded_command_bytes_size);
        }

        if (encode_timer.has_value()) {
            auto encode_ms = static_cast<f32>(encode_timer->elapsed_time().to_microseconds()) / 1000.f;
            PaintServer::dbgtrack("display list encode ms"sv, encode_ms, 5000);
        }

        m_stream.clear();
        return release_token.release_value();
    }();

    if (finalize_result.is_error() && m_stream.has_value()) {
        if (PaintServer::is_logging_enabled(PaintServer::LOG_INGRESS))
            dbgln("DisplayListSubmitter: finalize failed page_id={} frame_id={} error={}", m_sink_page_id, m_frame_index, finalize_result.error());
        m_stream->abort();
        m_stream.clear();
        return {};
    }

    return finalize_result.release_value();
}

ErrorOr<void> DisplayListSubmitter::append_display_list(DisplayList const& display_list, Optional<FlatPtr> source_context_namespace_id, ReadonlySpan<VisualContextRef const> inherited_context_nodes)
{
    auto const previous_scroll_state_snapshots = m_scroll_state_snapshots;
    auto const& nested_scroll_state_snapshots = display_list.scroll_state_snapshots_by_source_context_namespace_id();
    if (!nested_scroll_state_snapshots.is_empty())
        m_scroll_state_snapshots = &nested_scroll_state_snapshots;

    for (auto const& item : display_list.commands()) {
        VisualContextRef local_context;
        if (item.context_index.value())
            local_context = { &display_list.visual_context_tree(), item.context_index };

        if (local_context) {
            Vector<VisualContextRef> local_context_nodes = TRY(collect_context_nodes_root_to_leaf(local_context));
            for (auto const& context : local_context_nodes) {
                if (source_context_namespace_id.has_value())
                    TRY(m_context_to_source_context_namespace_id.try_set(context, *source_context_namespace_id));
            }
        }

        Vector<VisualContextRef> effective_context_nodes = TRY(extend_effective_context_nodes(inherited_context_nodes, local_context));
        if (item.kind == DisplayList::ItemKind::DrawCommand) {
            TRY(append_serialized_command(display_list.bytes_for(item), effective_context_nodes.span(), source_context_namespace_id));
            continue;
        }

        auto const& nested = display_list.nested_display_list_for(item);
        if (!nested.display_list)
            continue;

        PaintServer::SaveCommand save_command {};
        TRY(append_serialized_command(ReadonlyBytes { &save_command, sizeof(save_command) }, effective_context_nodes.span(), source_context_namespace_id));

        PaintServer::TranslateCommand translate_command { .delta = nested.rect.location().to_type<f32>() };
        TRY(append_serialized_command(ReadonlyBytes { &translate_command, sizeof(translate_command) }, effective_context_nodes.span(), source_context_namespace_id));

        Optional<FlatPtr> nested_source_context_namespace_id = nested.display_list->source_context_namespace_id();
        if (!nested_source_context_namespace_id.has_value())
            nested_source_context_namespace_id = source_context_namespace_id;
        TRY(append_display_list(*nested.display_list, nested_source_context_namespace_id, effective_context_nodes.span()));

        PaintServer::RestoreCommand restore_command {};
        TRY(append_serialized_command(ReadonlyBytes { &restore_command, sizeof(restore_command) }, effective_context_nodes.span(), source_context_namespace_id));
    }

    m_scroll_state_snapshots = previous_scroll_state_snapshots;
    return {};
}

ErrorOr<u32> DisplayListSubmitter::ensure_encoded_context_node(u32 encoded_parent, VisualContextRef context, Optional<FlatPtr> current_source_context_namespace_id)
{
    if (auto by_parent_it = m_encoded_context_index_by_parent.find(encoded_parent); by_parent_it != m_encoded_context_index_by_parent.end()) {
        if (auto encoded_index_it = by_parent_it->value.find(context); encoded_index_it != by_parent_it->value.end())
            return encoded_index_it->value;
    }

    auto const& node = context.tree->node_at(context.index);
    EncodedVisualContextNode encoded_node;
    encoded_node.parent_index = encoded_parent;
    encoded_node.has_empty_effective_clip = node.has_empty_effective_clip;
    encoded_node.kind = node.data.visit(
        [](ScrollData const&) { return PaintServer::VisualContextNodeKind::Scroll; },
        [](ClipData const&) { return PaintServer::VisualContextNodeKind::Clip; },
        [](TransformData const&) { return PaintServer::VisualContextNodeKind::Transform; },
        [](PerspectiveData const&) { return PaintServer::VisualContextNodeKind::Perspective; },
        [](ClipPathData const&) { return PaintServer::VisualContextNodeKind::ClipPath; },
        [](EffectsData const&) { return PaintServer::VisualContextNodeKind::Effects; });
    encoded_node.setup_commands = TRY(encode_context_setup_commands(context, current_source_context_namespace_id));

    u32 const encoded_index = static_cast<u32>(m_encoded_context_nodes.size());
    TRY(m_encoded_context_nodes.try_append(move(encoded_node)));
    auto& encoded_context_index_by_source = m_encoded_context_index_by_parent.ensure(encoded_parent, [] { return HashMap<VisualContextRef, u32> {}; });
    TRY(encoded_context_index_by_source.try_set(context, encoded_index));
    return encoded_index;
}

ErrorOr<ByteBuffer> DisplayListSubmitter::encode_context_setup_commands(VisualContextRef context, Optional<FlatPtr> current_source_context_namespace_id)
{
    auto maybe_context_encoding = page_encoding_context(m_sink_page_id, m_device_pixels_per_css_pixel);
    VERIFY(maybe_context_encoding.has_value());
    auto context_encoding = maybe_context_encoding.release_value();
    ByteBuffer setup_commands;
    CommandByteWriter append_bytes = [&](ReadonlyBytes bytes) -> ErrorOr<void> {
        return setup_commands.try_append(bytes);
    };
    auto const& node = context.tree->node_at(context.index);
    if (node.data.has<ScrollData>()) {
        PaintServer::SaveCommand save_command {};
        TRY(append_bytes(ReadonlyBytes { &save_command, sizeof(save_command) }));
        auto const scroll = node.data.get<ScrollData>();
        if (auto const* scroll_state_snapshot = scroll_state_snapshot_for(source_context_namespace_id_for(context, current_source_context_namespace_id))) {
            Gfx::FloatPoint offset = scroll_state_snapshot->device_offset_for_index(scroll.scroll_frame_index);
            if (!offset.is_zero()) {
                PaintServer::TranslateCommand translate_command { .delta = offset };
                TRY(append_bytes(ReadonlyBytes { &translate_command, sizeof(translate_command) }));
            }
        }
        return setup_commands;
    }

    if (node.data.has<EffectsData>()) {
        auto const& effects = node.data.get<EffectsData>();
        TRY(append_apply_effects_payload(append_bytes, context_encoding, effects.gfx_filter, effects.opacity, effects.blend_mode, {}));
        return setup_commands;
    }

    TRY(append_context_setup_commands(append_bytes, *context.tree, context.index));
    return setup_commands;
}

ErrorOr<void> DisplayListSubmitter::append_serialized_command(ReadonlyBytes command, ReadonlySpan<VisualContextRef const> effective_context_nodes, Optional<FlatPtr> current_source_context_namespace_id)
{
    u32 const visual_context_index = TRY(ensure_encoded_context_sequence(effective_context_nodes, current_source_context_namespace_id));
    u32 const command_offset = static_cast<u32>(m_payload_bytes_written);

    ReadonlyBytes bytes_to_append = command;
    Optional<ByteBuffer> rewritten_command;
    auto command_type = TRY(PaintServer::decode_draw_list_command_type(command));
    if (command_type == PaintServer::CommandType::PaintScrollBar) {
        auto scrollbar = TRY(PaintServer::read_command_struct<PaintServer::PaintScrollBarCommand>(command));
        if (auto const* scroll_state_snapshot = scroll_state_snapshot_for(current_source_context_namespace_id)) {
            Gfx::FloatPoint offset = scroll_state_snapshot->device_offset_for_index(ScrollFrameIndex { scrollbar.scroll_frame_id });
            auto rewritten_scrollbar = scrollbar;
            Gfx::IntRect thumb_rect = int_rect_from_float_rect(rewritten_scrollbar.thumb_rect);
            if (rewritten_scrollbar.vertical)
                thumb_rect.translate_by(0, static_cast<int>(-offset.y() * rewritten_scrollbar.scroll_size));
            else
                thumb_rect.translate_by(static_cast<int>(-offset.x() * rewritten_scrollbar.scroll_size), 0);
            rewritten_scrollbar.thumb_rect = thumb_rect.to_type<f32>();
            rewritten_command = TRY(ByteBuffer::copy(ReadonlyBytes { &rewritten_scrollbar, sizeof(rewritten_scrollbar) }));
            bytes_to_append = rewritten_command->bytes();
        }
    }

    TRY(append_payload_bytes(bytes_to_append));
    m_encoded_command_bytes_size += bytes_to_append.size();
    if (m_payload_bytes_written == command_offset)
        return {};
    TRY(m_command_records.try_append({
        .command_offset = command_offset,
        .visual_context_index = visual_context_index,
    }));
    return {};
}

void DisplayListSubmitter::invalidate_resource_for_page(u64 page_id, Gfx::ResourceID resource_id)
{
    auto* state = page_state(page_id);
    if (!state)
        return;

    switch (resource_id.type()) {
    case Gfx::WireResourceType::SkTypeface:
    case Gfx::WireResourceType::LocalFont:
        state->font_registry->invalidate_resource(resource_id);
        break;
    case Gfx::WireResourceType::Bitmap:
        state->bitmap_registry.invalidate_resource(resource_id);
        break;
    case Gfx::WireResourceType::Invalid:
        break;
    }
}

Optional<PaintServer::ResourceID> DisplayListSubmitter::register_bitmap_resource(u64 page_id, Gfx::DecodedImageFrame const& frame, Optional<u64> stable_bitmap_id)
{
    auto* state = page_state(page_id);
    if (!state)
        return {};

    u64 const bitmap_uid = stable_bitmap_id.value_or(static_cast<u64>(reinterpret_cast<FlatPtr>(frame.bitmap_ref().ptr())));
    auto it = state->bitmap_uid_to_resource_id.find(bitmap_uid);
    PaintServer::ResourceID stable_resource_id { 0 };
    if (it == state->bitmap_uid_to_resource_id.end()) {
        stable_resource_id = allocate_bitmap_resource_id();
        state->bitmap_uid_to_resource_id.set(bitmap_uid, stable_resource_id);
    } else {
        stable_resource_id = it->value;
    }

    auto image_resource_id = state->bitmap_registry.register_bitmap(stable_resource_id, frame);
    if (!image_resource_id.has_value()) {
        if (PaintServer::is_logging_enabled(PaintServer::LOG_RESOURCE)) {
            dbgln("DisplayListSubmitter: failed to register bitmap page_id={} resource_id={} bitmap_uid={} stable_uid={} size={}x{}",
                page_id,
                stable_resource_id,
                bitmap_uid,
                stable_bitmap_id.value_or(0),
                frame.size().width(),
                frame.size().height());
        }
        return {};
    }
    return image_resource_id.value();
}

Optional<PaintServer::ResourceID> DisplayListSubmitter::register_font_resource(u64 page_id, Gfx::Font const& font)
{
    auto* state = page_state(page_id);
    if (!state)
        return {};
    return state->font_registry->ensure_font_resource(font);
}

Optional<PaintServer::ReleaseToken> DisplayListSubmitter::submit_canvas_draw_list(u64 page_id, PaintServer::ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, ReadonlyBytes payload, Function<void(bool)> callback)
{
    auto* state = page_state(page_id);
    if (!state || !state->sink.has_value())
        return {};

    auto stream = state->sink.value()();
    if (!stream.has_value() || !stream->submit_canvas_draw_list)
        return {};

    auto release_token = stream->submit_canvas_draw_list(image_id, size, format, payload, take_pending_resource_submissions(*state), move(callback));
    if (release_token.has_value())
        state->pending_render_wait_sync_token = AK::max(state->pending_render_wait_sync_token, static_cast<PaintServer::SyncToken>(release_token.value()));
    return release_token;
}

Optional<FlatPtr> DisplayListSubmitter::source_context_namespace_id_for(VisualContextRef context, Optional<FlatPtr> current_source_context_namespace_id) const
{
    if (context) {
        if (auto it = m_context_to_source_context_namespace_id.find(context); it != m_context_to_source_context_namespace_id.end())
            return it->value;
    }
    return current_source_context_namespace_id;
}

ScrollStateSnapshot const* DisplayListSubmitter::scroll_state_snapshot_for(Optional<FlatPtr> source_context_namespace_id) const
{
    if (!source_context_namespace_id.has_value() || !m_scroll_state_snapshots)
        return nullptr;
    auto it = m_scroll_state_snapshots->find(*source_context_namespace_id);
    if (it == m_scroll_state_snapshots->end())
        return nullptr;
    return &it->value;
}

ErrorOr<u32> DisplayListSubmitter::ensure_encoded_context_sequence(ReadonlySpan<VisualContextRef const> effective_context_nodes, Optional<FlatPtr> current_source_context_namespace_id)
{
    u32 encoded_parent = 0;
    for (auto const& context : effective_context_nodes) {
        if (!context)
            continue;
        encoded_parent = TRY(ensure_encoded_context_node(encoded_parent, context, current_source_context_namespace_id));
    }
    return encoded_parent;
}

ErrorOr<u32> DisplayListSubmitter::append_payload_bytes(ReadonlyBytes bytes)
{
    u32 const offset = static_cast<u32>(m_payload_bytes_written);
    if (!bytes.is_empty())
        TRY(m_stream->append_payload(offset, bytes));
    m_payload_bytes_written += bytes.size();
    return offset;
}

void DisplayListSubmitter::set_sink(u64 page_id, DisplayListStreamSink sink)
{
    ensure_page_state(page_id).sink = move(sink);
}

void DisplayListSubmitter::clear_sink(u64 page_id)
{
    page_states().remove(page_id);
}

void DisplayListSubmitter::reset_all_resources()
{
    for (auto& it : page_states())
        it.value->reset();
}

void DisplayListSubmitter::reset_resources_for_page(u64 page_id)
{
    if (auto* state = page_state(page_id))
        state->reset();
}

static Vector<Gfx::ResourceTransfer> take_pending_resource_submissions(DisplayListSubmitter::PageState& page_state)
{
    Vector<Gfx::ResourceTransfer> resource_submissions;
    Vector<Gfx::ResourceTransfer> font_submissions = page_state.font_registry->take_pending_transfers();
    Vector<Gfx::ResourceTransfer> bitmap_submissions = page_state.bitmap_registry.take_pending_transfers();
    resource_submissions.ensure_capacity(font_submissions.size() + bitmap_submissions.size());
    for (auto& submission : font_submissions) {
        if (submission.info.resource_id == 0)
            continue;
        if (submission.bytes.is_empty() && !submission.shared_image_payload)
            continue;
        resource_submissions.append(move(submission));
    }
    for (auto& submission : bitmap_submissions) {
        if (submission.info.resource_id == 0)
            continue;
        if (submission.bytes.is_empty() && !submission.shared_image_payload)
            continue;
        resource_submissions.append(move(submission));
    }
    return resource_submissions;
}

static DisplayListSubmitter::PageState* page_state(u64 page_id)
{
    auto it = page_states().find(page_id);
    if (it == page_states().end())
        return nullptr;
    return it->value.ptr();
}

static HashMap<u64, NonnullOwnPtr<DisplayListSubmitter::PageState>>& page_states()
{
    static HashMap<u64, NonnullOwnPtr<DisplayListSubmitter::PageState>> s_page_states;
    return s_page_states;
}

static DisplayListSubmitter::PageState& ensure_page_state(u64 page_id)
{
    auto& page_state = page_states().ensure(page_id, [] { return make<DisplayListSubmitter::PageState>(); });
    return *page_state;
}

}
