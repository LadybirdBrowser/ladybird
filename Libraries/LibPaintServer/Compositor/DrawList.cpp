/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPaintServer/Compositor/DrawCommands.h>
#include <LibPaintServer/Compositor/DrawList.h>

namespace PaintServer {

struct CommandHeader {
    u32 command_type { 0 };
    u32 command_size { 0 };
};

ErrorOr<Optional<DrawCommandView>> Cursor::next()
{
    if (is_eof())
        return OptionalNone {};

    CommandHeader header = TRY(read_command_struct<CommandHeader>(m_payload, m_offset));

    if (header.command_size < sizeof(CommandHeader))
        return Error::from_string_literal("Draw-list command has invalid encoded size");

    size_t const command_size = static_cast<size_t>(header.command_size);
    if (m_payload.size() - m_offset < command_size)
        return Error::from_string_literal("Draw-list payload is truncated");

    ReadonlyBytes command_bytes = m_payload.slice(m_offset, command_size);
    m_offset += command_size;

    auto command_type = TRY(decode_draw_list_command_type(command_bytes));
    return DrawCommandView { command_type, command_bytes };
}

static ErrorOr<Vector<DrawCommandView>> scan_commands_for_payload(ReadonlyBytes payload)
{
    Vector<DrawCommandView> commands;

    Cursor cursor(payload);
    size_t offset = 0;
    for (;;) {
        auto maybe_command = TRY(cursor.next());
        if (!maybe_command.has_value())
            break;
        auto const& command = maybe_command.value();
        TRY(commands.try_append(command));
        offset += command.bytes.size();
    }
    if (offset != payload.size())
        return Error::from_string_literal("Draw-list scan did not consume entire payload");

    return commands;
}

ErrorOr<DrawList> DrawList::copy(ReadonlyBytes payload)
{
    DrawList segment;
    segment.m_payload = TRY(ByteBuffer::copy(payload));
    return segment;
}

ErrorOr<void> DrawList::try_append_command(ReadonlyBytes command_bytes)
{
    if (command_bytes.size() < sizeof(CommandHeader))
        return Error::from_string_literal("Draw-list command is too small for command header");

    CommandHeader header;
    AK::TypedTransfer<u8>::copy(reinterpret_cast<u8*>(&header), command_bytes.data(), sizeof(header));

    if (header.command_size < sizeof(CommandHeader))
        return Error::from_string_literal("Draw-list command has invalid encoded size");
    if (static_cast<size_t>(header.command_size) != command_bytes.size())
        return Error::from_string_literal("Draw-list command size does not match payload size");

    (void)TRY(decode_draw_list_command_type(command_bytes));

    return m_payload.try_append(command_bytes);
}

ErrorOr<Vector<DrawCommandView>> DrawList::scan_commands() const
{
    return scan_commands_for_payload(bytes());
}

ErrorOr<CommandType> decode_draw_list_command_type(ReadonlyBytes payload)
{
    if (payload.size() < sizeof(u32))
        return Error::from_string_literal("Draw-list payload is too short for command type");

    u32 raw_command_type = TRY(read_command_struct<u32>(payload));

    if (raw_command_type == 0 || raw_command_type >= to_underlying(CommandType::Count))
        return Error::from_string_literal("Draw-list payload has unsupported command type");

    CommandType command_type = static_cast<CommandType>(raw_command_type);
    return command_type;
}

}
