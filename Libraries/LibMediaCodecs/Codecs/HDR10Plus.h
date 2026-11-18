/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibMediaCodecs/Export.h>

namespace Media::Codecs {

// The largest HDR10+ ITU-T T.35 message, from its country code: the six bytes up to its application identifier, then
// the 907 that FFmpeg's AV_HDR_PLUS_MAX_PAYLOAD_SIZE allows for the rest.
constexpr size_t HDR10_PLUS_MAXIMUM_T35_MESSAGE_SIZE = 6 + 907;

// The brightest colour component in the scene in cd/m², from an HDR10+ (SMPTE ST 2094-40) ITU-T T.35 message that
// begins with its country code. Empty for any other message, or one that gives no peak.
MEDIACODECS_API Optional<float> hdr10_plus_maximum_scene_luminance(ReadonlyBytes t35_message);

}
