/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/StringView.h>

namespace RendererSandbox {

// NB: No renderer reaches the audio stack: playback, capture and device enumeration all go through the
//     AudioServer. On Linux, reaching the audio server would mean the whole UNIX socket namespace.
[[nodiscard]] ErrorOr<void> apply_sandbox(StringView mach_server_name);

}
