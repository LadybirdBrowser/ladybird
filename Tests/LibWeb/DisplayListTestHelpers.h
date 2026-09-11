/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibWeb/Painting/DisplayList.h>

// Appends one record the way the Rust builder writes it: header, payload, then zero padding up to
// the command alignment.
template<Web::Painting::DisplayListCommand Command>
void append_display_list_command(ByteBuffer& command_bytes, Command const& command, Optional<Gfx::IntRect> bounding_rect = {}, Web::Painting::ContextRef context = {})
{
    auto payload = Web::Painting::display_list_object_bytes(command);
    auto record_size = sizeof(Web::Painting::DisplayListCommandHeader) + payload.size();
    auto payload_size = align_up_to(record_size, Web::Painting::DisplayList::command_alignment) - sizeof(Web::Painting::DisplayListCommandHeader);
    Web::Painting::DisplayListCommandHeader header {
        .command_type = Command::command_type,
        .has_bounding_rect = bounding_rect.has_value(),
        .inline_clip_count = 0,
        .has_inline_transform = false,
        .payload_size = static_cast<u32>(payload_size),
        .context = context,
        .bounding_rect = bounding_rect.value_or({}),
    };
    command_bytes.append(Web::Painting::display_list_object_bytes(header));
    command_bytes.append(payload);
    command_bytes.resize(align_up_to(command_bytes.size(), Web::Painting::DisplayList::command_alignment), ByteBuffer::ZeroFillNewElements::Yes);
}

// Round-trips a freshly built display list through the IPC encoder, so the receiver gets it the way the compositor
// process would.
inline NonnullRefPtr<Web::Painting::DisplayList> decode_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, ByteBuffer command_bytes, Optional<Gfx::Color> surface_clear_color = {}, Optional<Web::Painting::DisplayList::AsyncScrollingMetadata> async_scrolling_metadata = {})
{
    auto command_runs = Web::Painting::compute_display_list_command_runs(command_bytes);
    auto display_list = Web::Painting::DisplayList::create_from_command_bytes(visual_context_tree, move(command_bytes), move(command_runs));
    if (surface_clear_color.has_value())
        display_list->set_surface_clear_color(*surface_clear_color);
    if (async_scrolling_metadata.has_value())
        display_list->set_async_scrolling_metadata(*async_scrolling_metadata);

    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(*display_list));

    FixedMemoryStream stream { buffer.data().span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return MUST(decoder.decode<NonnullRefPtr<Web::Painting::DisplayList>>());
}
