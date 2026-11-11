/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/Canvas2DTape.h>
#include <LibCompositing/RustFFI.h>

namespace Compositing {

ErrorOr<void> validate_canvas_2d_tape(ReadonlyBytes tape)
{
    u8 const* error = nullptr;
    size_t error_size = 0;
    if (!RustFFI::canvas_2d_tape_validate(tape, &error, &error_size))
        return Error::from_string_view({ reinterpret_cast<char const*>(error), error_size });
    return {};
}

void play_canvas_2d_tape(ReadonlyBytes tape, Function<void(DisplayListCommandType, void const* command, PlayedPayload const&)> const& play)
{
    RustFFI::canvas_2d_tape_play(tape, const_cast<void*>(static_cast<void const*>(&play)), [](void* context, DisplayListCommandType command_type, void const* command, PlayedPayload const* payload) {
        (*static_cast<Function<void(DisplayListCommandType, void const*, PlayedPayload const&)> const*>(context))(command_type, command, *payload);
    });
}

}
