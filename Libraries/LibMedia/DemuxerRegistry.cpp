/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/Containers/ADTS/ADTSDemuxer.h>
#include <LibMedia/Containers/MP3/MP3Demuxer.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/MediaStream.h>

namespace Media {

using DemuxerFactory = DecoderErrorOr<NonnullRefPtr<Demuxer>> (*)(NonnullRefPtr<MediaStream> const&);
using SupportsContainerMimeTypeFunction = bool (*)(ContainerMimeType);
using SupportsCodecInContainerFunction = bool (*)(ContainerID, CodecID);

struct DemuxerRegistration {
    DemuxerFactory create;
    SupportsContainerMimeTypeFunction supports_container_mime_type;
    SupportsCodecInContainerFunction supports_codec_in_container;
};

static constexpr Array demuxers_in_priority_order {
    DemuxerRegistration { Matroska::MatroskaDemuxer::from_stream, Matroska::MatroskaDemuxer::supports_container_mime_type, Matroska::MatroskaDemuxer::supports_codec_in_container },
    DemuxerRegistration { FFmpeg::FFmpegDemuxer::from_stream, FFmpeg::FFmpegDemuxer::supports_container_mime_type, FFmpeg::FFmpegDemuxer::supports_codec_in_container },
    DemuxerRegistration { MP3::MP3Demuxer::from_stream, MP3::MP3Demuxer::supports_container_mime_type, MP3::MP3Demuxer::supports_codec_in_container },
    DemuxerRegistration { ADTS::ADTSDemuxer::from_stream, ADTS::ADTSDemuxer::supports_container_mime_type, ADTS::ADTSDemuxer::supports_codec_in_container },
};

bool is_supported_file_container(ContainerMimeType mime_type)
{
    return any_of(demuxers_in_priority_order, [&](auto const& registration) {
        return registration.supports_container_mime_type(mime_type);
    });
}

bool is_codec_supported_in_file_container(ContainerMimeType mime_type, CodecID codec_id)
{
    if (mime_type.media_type == ContainerMediaType::Audio && track_type_from_codec_id(codec_id) != TrackType::Audio)
        return false;

    return any_of(demuxers_in_priority_order, [&](auto const& registration) {
        return registration.supports_container_mime_type(mime_type)
            && registration.supports_codec_in_container(mime_type.container_id, codec_id);
    });
}

DecoderErrorOr<NonnullRefPtr<Demuxer>> create_demuxer(NonnullRefPtr<MediaStream> const& stream)
{
    for (auto const& registration : demuxers_in_priority_order) {
        auto demuxer_or_error = registration.create(stream);
        if (!demuxer_or_error.is_error())
            return demuxer_or_error;
        if (demuxer_or_error.error().category() != DecoderErrorCategory::UnrecognizedFormat)
            return demuxer_or_error.release_error();
    }

    return DecoderError::with_description(DecoderErrorCategory::UnrecognizedFormat, "Could not find a demuxer for stream"sv);
}

}
