/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibMediaCodecs/Export.h>

namespace Media::FFmpeg {

// Loads the FFmpeg that Ladybird is built with, which a process does before entering its sandbox and before decoding,
// encoding or converting anything through it.
MEDIACODECS_API ErrorOr<void> load_bundled_ffmpeg();

}
