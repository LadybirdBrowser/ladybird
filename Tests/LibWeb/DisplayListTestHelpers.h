/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibWeb/Painting/DisplayList.h>

// Appends one record the way the Rust builder writes it: header, payload, then zero padding up to
// the command alignment.
template<Web::Painting::DisplayListCommand Command>
void append_display_list_command(ByteBuffer& command_bytes, Command const& command, Optional<Gfx::IntRect> bounding_rect = {}, Web::Painting::ContextRef context = {}, bool is_clip = false)
{
    auto payload = Web::Painting::display_list_object_bytes(command);
    auto record_size = sizeof(Web::Painting::DisplayListCommandHeader) + payload.size();
    auto payload_size = align_up_to(record_size, Web::Painting::DisplayList::command_alignment) - sizeof(Web::Painting::DisplayListCommandHeader);
    Web::Painting::DisplayListCommandHeader header {
        .command_type = Command::command_type,
        .has_bounding_rect = bounding_rect.has_value(),
        .is_clip = is_clip,
        .payload_size = static_cast<u32>(payload_size),
        .context = context,
        .bounding_rect = bounding_rect.value_or({}),
    };
    command_bytes.append(Web::Painting::display_list_object_bytes(header));
    command_bytes.append(payload);
    command_bytes.resize(align_up_to(command_bytes.size(), Web::Painting::DisplayList::command_alignment), ByteBuffer::ZeroFillNewElements::Yes);
}
