/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibMedia/Export.h>

namespace Media::ID3 {

static constexpr size_t VERSION_2_HEADER_SIZE = 10;

// The bytes an ID3v2 tag occupies, for streams that carry one ahead of the audio a demuxer is looking for.
MEDIA_API Optional<size_t> version_2_tag_size(ReadonlyBytes);

}
