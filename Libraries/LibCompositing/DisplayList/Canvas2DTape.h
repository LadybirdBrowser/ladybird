/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Span.h>
#include <LibCompositing/DisplayList/DisplayListCommandsGenerated.h>
#include <LibCompositing/Export.h>

namespace Compositing {

// Checks the tape of a segment that came from WebContent.
COMPOSITING_API ErrorOr<void> validate_canvas_2d_tape(ReadonlyBytes);

// Plays the commands of a checked tape in order, calling `play` with the type of each, the command, whose type the
// command type names, and what its payload holds.
COMPOSITING_API void play_canvas_2d_tape(ReadonlyBytes tape, Function<void(DisplayListCommandType, void const* command, PlayedPayload const&)> const& play);

}
