/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibMedia/Export.h>
#include <LibMediaCodecs/AudioDecoderRegistry.h>
#include <LibMediaCodecs/VideoDecoderRegistry.h>

namespace Media {

MEDIA_API ReadonlySpan<VideoDecoderRegistration> hardware_video_decoders();
MEDIA_API ReadonlySpan<VideoDecoderRegistration> ffmpeg_video_decoders();
MEDIA_API ReadonlySpan<AudioDecoderRegistration> audio_decoders();

}
