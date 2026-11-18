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

// The brightest light in the scene in cd/m², from the level 1 metadata of a Dolby Vision RPU. The RPU is unescaped and
// begins with its 0x19 prefix, which is how an H.265 NAL unit of type 62 carries it after its header. Empty for an RPU
// that carries no display management metadata.
MEDIACODECS_API Optional<float> dolby_vision_maximum_scene_luminance(ReadonlyBytes rpu);

// The same, from an ITU-T T.35 message that begins with its country code, which is how an AV1 metadata OBU carries an
// RPU. Empty for any other message.
MEDIACODECS_API Optional<float> dolby_vision_maximum_scene_luminance_of_t35(ReadonlyBytes t35_message);

}
