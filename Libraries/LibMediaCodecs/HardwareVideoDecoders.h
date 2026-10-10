/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/VideoDecoderRegistry.h>

namespace Media {

MEDIACODECS_API ReadonlySpan<VideoDecoderRegistration> hardware_video_decoders();

}
