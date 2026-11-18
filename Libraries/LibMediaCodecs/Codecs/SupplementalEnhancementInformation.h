/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibMediaCodecs/Color/ColorMetadata.h>

namespace Media::Codecs {

// The payload is an H.264 or H.265 SEI NAL unit without its header, with its emulation prevention bytes still in place.
ColorMetadata high_dynamic_range_metadata_of_sei_payload(ReadonlyBytes escaped_sei_payload);

}
