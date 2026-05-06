/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Vector.h>
#include <LibCore/ElapsedTimer.h>
#include <LibPaintServer/Compositor/DisplayListPayload.h>
#include <LibPaintServer/Compositor/DrawCommands.h>
#include <LibPaintServer/Compositor/DrawList.h>
#include <LibPaintServer/Debug.h>
#include <PaintServer/Compositor/DisplayListPlayer.h>

#include <core/SkCanvas.h>

namespace PaintServer {

class VisualContextReplayer {
public:
    VisualContextReplayer(DrawContext const&, ReadonlyBytes payload, ReadonlySpan<VisualContextNodeRecord const>, SkCanvas&);

    ErrorOr<void> initialize_depths();
    ErrorOr<void> apply_command_record(DisplayListCommandRecord const&);

private:
    ErrorOr<void> apply_context_setup(u32 context_index);
    ErrorOr<void> restore_one_context();
    u32 find_common_ancestor(u32 left, u32 right) const;
    ErrorOr<void> switch_to_context(u32 target_index);

    ReadonlyBytes m_payload;
    ReadonlySpan<VisualContextNodeRecord const> m_node_records;
    DrawCommandPlayer m_player;
    Vector<u32> m_depths;
    u32 m_current_context_index { 0 };
};

static ErrorOr<ReadonlyBytes> checked_payload_bytes(ReadonlyBytes payload, u32 offset, u32 size)
{
    Checked<size_t> end = offset;
    end += size;
    if (end.has_overflow() || end.value() > payload.size())
        return Error::from_string_literal("DisplayListPlayer payload range is out of bounds");
    return payload.slice(offset, size);
}

static ErrorOr<DrawCommandView> command_view_at(ReadonlyBytes payload, u32 offset)
{
    if (offset >= payload.size())
        return Error::from_string_literal("DisplayListPlayer command offset is out of bounds");

    Cursor cursor(payload.slice(offset, payload.size() - offset));
    auto maybe_command = TRY(cursor.next());
    if (!maybe_command.has_value())
        return Error::from_string_literal("DisplayListPlayer command stream ended unexpectedly");
    return maybe_command.release_value();
}

ErrorOr<void> paint_display_list_payload(DrawContext const& draw_context, ReadonlyBytes payload, DisplayListPayloadFooter const& footer, SkCanvas& canvas)
{
    Optional<Core::ElapsedTimer> paint_timer;
    if (is_logging_enabled(LOG_TIMING))
        paint_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    size_t const command_records_size = static_cast<size_t>(footer.command_record_count) * sizeof(DisplayListCommandRecord);
    ReadonlyBytes command_record_bytes = TRY(checked_payload_bytes(payload, footer.command_records_offset, static_cast<u32>(command_records_size)));
    ReadonlySpan<DisplayListCommandRecord const> command_records { reinterpret_cast<DisplayListCommandRecord const*>(command_record_bytes.data()), footer.command_record_count };

    size_t const node_records_size = static_cast<size_t>(footer.visual_context_node_count) * sizeof(VisualContextNodeRecord);
    ReadonlyBytes node_record_bytes = TRY(checked_payload_bytes(payload, footer.visual_context_nodes_offset, static_cast<u32>(node_records_size)));
    ReadonlySpan<VisualContextNodeRecord const> node_records { reinterpret_cast<VisualContextNodeRecord const*>(node_record_bytes.data()), footer.visual_context_node_count };
    if (node_records.is_empty())
        return Error::from_string_literal("DisplayListPlayer visual context table is empty");

    if (command_records.is_empty()) {
        dbgln("DisplayListPlayer: bad frame reason=no_command_records surface_id={} payload_bytes={} command_stream_size={} visual_context_nodes={}",
            draw_context.surface_id,
            payload.size(),
            footer.command_stream_size,
            node_records.size());
    }
    size_t drawable_command_count = 0;
    for (auto const& command_record : command_records) {
        auto command = TRY(command_view_at(payload, command_record.command_offset));
        if (!is_state_command(command.type))
            ++drawable_command_count;
    }
    if (!command_records.is_empty() && drawable_command_count == 0) {
        dbgln("DisplayListPlayer: bad frame reason=no_drawable_commands surface_id={} payload_bytes={} command_records={} command_stream_size={} visual_context_nodes={}",
            draw_context.surface_id,
            payload.size(),
            command_records.size(),
            footer.command_stream_size,
            node_records.size());
    }
    VisualContextReplayer display_list_player(draw_context, payload, node_records, canvas);
    TRY(display_list_player.initialize_depths());

    if (is_logging_enabled(LOG_INGRESS)) {
        dbgln("DisplayListPlayer: decode surface_id={} payload_bytes={} command_records={} visual_context_nodes={}",
            draw_context.surface_id,
            payload.size(),
            command_records.size(),
            node_records.size());
    }
    for (auto const& command_record : command_records)
        TRY(display_list_player.apply_command_record(command_record));

    if (paint_timer.has_value()) {
        dbgtrack("paint ms"sv,
            static_cast<f32>(paint_timer->elapsed_time().to_microseconds()) / 1000.f,
            5000);
    }
    return {};
}

static bool has_valid_payload_footer(ReadonlyBytes payload, DisplayListPayloadFooter const& footer)
{
    if (footer.total_payload_bytes != payload.size() || footer.command_stream_offset != 0)
        return false;

    Checked<size_t> command_stream_end = footer.command_stream_offset;
    command_stream_end += footer.command_stream_size;
    if (command_stream_end.has_overflow() || command_stream_end.value() > payload.size())
        return false;

    Checked<size_t> command_records_end = footer.command_records_offset;
    command_records_end += static_cast<size_t>(footer.command_record_count) * sizeof(DisplayListCommandRecord);
    if (command_records_end.has_overflow() || command_records_end.value() > payload.size())
        return false;

    Checked<size_t> visual_context_nodes_end = footer.visual_context_nodes_offset;
    visual_context_nodes_end += static_cast<size_t>(footer.visual_context_node_count) * sizeof(VisualContextNodeRecord);
    if (visual_context_nodes_end.has_overflow() || visual_context_nodes_end.value() > payload.size())
        return false;

    Checked<size_t> scroll_frames_end = footer.scroll_frames_offset;
    scroll_frames_end += static_cast<size_t>(footer.scroll_frame_count) * sizeof(DisplayListScrollFrameRecord);
    if (scroll_frames_end.has_overflow())
        return false;

    return scroll_frames_end.value() <= payload.size();
}

ErrorOr<DisplayListPayloadFooter> validate_display_list_payload(ReadonlyBytes payload)
{
    if (payload.size() < sizeof(DisplayListPayloadFooter))
        return Error::from_string_literal("DisplayListPlayer payload is too small");

    auto const& footer = *reinterpret_cast<DisplayListPayloadFooter const*>(payload.data() + payload.size() - sizeof(DisplayListPayloadFooter));
    if (!has_valid_payload_footer(payload, footer))
        return Error::from_string_literal("DisplayListPlayer payload footer is invalid");

    return footer;
}

VisualContextReplayer::VisualContextReplayer(DrawContext const& draw_context, ReadonlyBytes payload, ReadonlySpan<VisualContextNodeRecord const> node_records, SkCanvas& canvas)
    : m_payload(payload)
    , m_node_records(node_records)
    , m_player(draw_context, canvas)
{
}

ErrorOr<void> VisualContextReplayer::initialize_depths()
{
    TRY(m_depths.try_resize(m_node_records.size()));
    for (size_t index = 1; index < m_node_records.size(); ++index) {
        auto const& node = m_node_records[index];
        if (node.parent_index >= index)
            return Error::from_string_literal("DisplayListPlayer visual context parent index is invalid");
        m_depths[index] = m_depths[node.parent_index] + 1;
    }
    return {};
}

ErrorOr<void> VisualContextReplayer::apply_command_record(DisplayListCommandRecord const& command_record)
{
    if (command_record.visual_context_index >= m_node_records.size())
        return Error::from_string_literal("DisplayListPlayer visual context index is invalid");

    TRY(switch_to_context(command_record.visual_context_index));
    return m_player.apply(TRY(command_view_at(m_payload, command_record.command_offset)));
}

ErrorOr<void> VisualContextReplayer::apply_context_setup(u32 context_index)
{
    auto const& node_record = m_node_records[context_index];
    if (node_record.data_size == 0)
        return {};

    Cursor cursor(TRY(checked_payload_bytes(m_payload, node_record.data_offset, node_record.data_size)));
    for (;;) {
        auto maybe_command = TRY(cursor.next());
        if (!maybe_command.has_value())
            break;
        TRY(m_player.apply(maybe_command.release_value()));
    }
    return {};
}

ErrorOr<void> VisualContextReplayer::restore_one_context()
{
    RestoreCommand restore_command {};
    return m_player.apply({
        .type = CommandType::Restore,
        .bytes = ReadonlyBytes { &restore_command, sizeof(restore_command) },
    });
}

u32 VisualContextReplayer::find_common_ancestor(u32 left, u32 right) const
{
    u32 lhs = left;
    u32 rhs = right;
    while (m_depths[lhs] > m_depths[rhs])
        lhs = m_node_records[lhs].parent_index;
    while (m_depths[rhs] > m_depths[lhs])
        rhs = m_node_records[rhs].parent_index;
    while (lhs != rhs) {
        lhs = m_node_records[lhs].parent_index;
        rhs = m_node_records[rhs].parent_index;
    }
    return lhs;
}

ErrorOr<void> VisualContextReplayer::switch_to_context(u32 target_index)
{
    if (m_current_context_index == target_index)
        return {};

    u32 const common_ancestor = find_common_ancestor(m_current_context_index, target_index);
    while (m_current_context_index != common_ancestor) {
        TRY(restore_one_context());
        m_current_context_index = m_node_records[m_current_context_index].parent_index;
    }
    Vector<u32> nodes_to_apply;
    for (u32 context_index = target_index; context_index != common_ancestor; context_index = m_node_records[context_index].parent_index)
        TRY(nodes_to_apply.try_append(context_index));
    for (size_t index = nodes_to_apply.size(); index-- > 0;)
        TRY(apply_context_setup(nodes_to_apply[index]));

    m_current_context_index = target_index;
    return {};
}

}
