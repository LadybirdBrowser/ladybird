/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/StringView.h>

namespace RendererSandbox {

// NB: Only the renderer that hosts a Window plays or captures audio. Granting a renderer access to
//     the audio server also grants it the whole UNIX socket namespace, so WebWorker must not ask
//     for it.
enum class AudioAccess {
    No,
    Yes,
};

[[nodiscard]] ErrorOr<void> apply_sandbox(StringView mach_server_name, Optional<StringView> cache_path, AudioAccess);

}
