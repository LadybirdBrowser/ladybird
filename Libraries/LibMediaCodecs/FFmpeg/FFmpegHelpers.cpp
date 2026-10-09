/*
 * Copyright (c) 2024-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>

#include "FFmpegHelpers.h"

namespace Media::FFmpeg {

DecoderErrorOr<void> set_codec_initialization_data(FFmpegFunctions const& functions, AVCodecContext& codec_context, CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    if (codec_initialization_data.is_empty())
        return {};
    if (codec_initialization_data.size() > NumericLimits<int>::max())
        return DecoderError::corrupted("Codec initialization data is too large"sv);

    auto* parameters = functions.avcodec_parameters_alloc();
    if (!parameters)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg codec parameters"sv);
    ScopeGuard free_parameters = [&] { functions.avcodec_parameters_free(&parameters); };

    parameters->codec_type = ffmpeg_media_type_from_track_type(track_type_from_codec_id(codec_id));
    parameters->codec_id = functions.ffmpeg_codec_id(codec_id);
    parameters->extradata = static_cast<u8*>(functions.av_malloc(codec_initialization_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!parameters->extradata)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate codec initialization data buffer for FFmpeg codec"sv);
    parameters->extradata_size = static_cast<int>(codec_initialization_data.size());
    codec_initialization_data.copy_to({ parameters->extradata, codec_initialization_data.size() });
    memset(parameters->extradata + codec_initialization_data.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);

    if (functions.avcodec_parameters_to_context(&codec_context, parameters) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to apply codec initialization data to FFmpeg codec"sv);
    return {};
}

DecoderErrorOr<void> add_new_extradata_to_packet(FFmpegFunctions const& functions, AVPacket& packet, ReadonlyBytes new_extradata)
{
    auto* packet_extradata = functions.av_packet_new_side_data(&packet, AV_PKT_DATA_NEW_EXTRADATA, new_extradata.size());
    if (!packet_extradata)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet extradata"sv);
    new_extradata.copy_to({ packet_extradata, new_extradata.size() });
    return {};
}

}
