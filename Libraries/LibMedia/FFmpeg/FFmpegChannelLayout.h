/*
 * Copyright (c) 2024-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibAudio/ChannelMap.h>
#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>

extern "C" {
#include <libavutil/channel_layout.h>
}

namespace Media::FFmpeg {

ErrorOr<Audio::ChannelMap> av_channel_layout_to_channel_map(FFmpegFunctions const&, AVChannelLayout const&);
ErrorOr<AVChannelLayout> channel_map_to_av_channel_layout(FFmpegFunctions const&, Audio::ChannelMap const&);

}
