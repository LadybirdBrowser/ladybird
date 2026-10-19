/*
 * Copyright (c) 2024-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibMediaCodecs/CodedFrame.h>

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

// Frames that FFmpeg never output, such as ones it drops for corruption, stop being kept once this many later ones
// have gone in.
static constexpr u64 MAXIMUM_FRAMES_AWAITING_OUTPUT = 256;

DecoderErrorOr<u64> VideoFramesAwaitingOutput::add(CodedFrame const& coded_frame, DecodeIntent intent)
{
    auto key = m_next_key++;
    if (m_frames.size() >= MAXIMUM_FRAMES_AWAITING_OUTPUT) {
        auto stale_frame_count = m_frames.size();
        m_frames.remove_all_matching([&](u64 frame_key, auto const&) { return frame_key + MAXIMUM_FRAMES_AWAITING_OUTPUT <= key; });
        stale_frame_count -= m_frames.size();
        if (stale_frame_count > 0)
            dbgln("FFmpeg: {} frames were never output", stale_frame_count);
    }
    DECODER_TRY_ALLOC(m_frames.try_set(key, { coded_frame.presentation_timestamp(), coded_frame.duration(), intent }));
    return key;
}

Optional<VideoFramesAwaitingOutput::Frame> VideoFramesAwaitingOutput::take(i64 key)
{
    return m_frames.take(static_cast<u64>(key));
}

DecoderErrorOr<void> send_coded_frame(FFmpegFunctions const& functions, AVCodecContext& codec_context, AVPacket& packet, CodedFrame const& coded_frame, u64 key)
{
    auto coded_data = coded_frame.data();
    VERIFY(coded_data.size() < NumericLimits<int>::max());

    packet.data = const_cast<u8*>(coded_data.data());
    packet.size = static_cast<int>(coded_data.size());
    packet.pts = static_cast<i64>(key);
    packet.dts = AV_NOPTS_VALUE;

    ScopeGuard clear_packet_side_data { [&] { functions.av_packet_free_side_data(&packet); } };
    auto new_codec_configuration = coded_frame.new_codec_configuration();
    if (new_codec_configuration.has_value() && !new_codec_configuration->is_empty())
        TRY(add_new_extradata_to_packet(functions, packet, *new_codec_configuration));

    auto result = functions.avcodec_send_packet(&codec_context, &packet);
    switch (result) {
    case 0:
        return {};
    case AVERROR(EAGAIN):
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "FFmpeg decoder cannot decode any more data until frames have been retrieved"sv);
    case AVERROR_EOF:
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
    case AVERROR(EINVAL):
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg codec has not been opened"sv);
    case AVERROR(ENOMEM):
        return DecoderError::with_description(DecoderErrorCategory::Memory, "FFmpeg codec ran out of internal memory"sv);
    default:
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "FFmpeg codec reports that the data is corrupted"sv);
    }
}

CodingIndependentCodePoints coding_independent_code_points_of(FFmpegFunctions const& functions, AVCodecContext& codec_context, CodingIndependentCodePoints const& container_cicp)
{
    auto option = [&](char const* name) {
        i64 value = 0;
        VERIFY(functions.av_opt_get_int(&codec_context, name, 0, &value) == 0);
        return value;
    };
    auto color_primaries = static_cast<ColorPrimaries>(option("color_primaries"));
    auto transfer_characteristics = static_cast<TransferCharacteristics>(option("color_trc"));
    auto matrix_coefficients = static_cast<MatrixCoefficients>(option("colorspace"));
    auto color_range = [&] {
        switch (option("color_range")) {
        case AVColorRange::AVCOL_RANGE_MPEG:
            return VideoFullRangeFlag::Studio;
        case AVColorRange::AVCOL_RANGE_JPEG:
            return VideoFullRangeFlag::Full;
        default:
            return VideoFullRangeFlag::Unspecified;
        }
    }();
    auto cicp = container_cicp;
    cicp.adopt_specified_values({ color_primaries, transfer_characteristics, matrix_coefficients, color_range });
    return cicp;
}

}
