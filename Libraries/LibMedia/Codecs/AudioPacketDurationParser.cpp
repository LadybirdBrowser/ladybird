/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Codecs/AudioPacketDurationParser.h>
#include <LibMedia/ReadonlyBytesCursor.h>

namespace Media::Codecs {

DecoderErrorOr<AudioPacketDurationParser> AudioPacketDurationParser::create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    switch (codec_id) {
    case CodecID::Opus:
        return AudioPacketDurationParser { Opus::PacketDurationParser {} };
    case CodecID::Vorbis:
        return AudioPacketDurationParser { TRY(Vorbis::PacketDurationParser::create(codec_initialization_data)) };
    case CodecID::FLAC:
        return AudioPacketDurationParser { TRY(FLAC::PacketDurationParser::create(codec_initialization_data)) };
    default:
        return AudioPacketDurationParser {};
    }
}

u32 AudioPacketDurationParser::samples_per_second() const
{
    return m_implementation.visit(
        [](Empty) -> u32 { VERIFY_NOT_REACHED(); },
        [](auto const& implementation) { return implementation.samples_per_second(); });
}

DecoderErrorOr<u32> AudioPacketDurationParser::parse_duration_in_samples(MediaStreamCursor& cursor, size_t packet_size)
{
    return m_implementation.visit(
        [](Empty) -> DecoderErrorOr<u32> { return DecoderError::not_implemented(); },
        [&](auto& implementation) { return implementation.parse_duration_in_samples(cursor, packet_size); });
}

DecoderErrorOr<u32> AudioPacketDurationParser::parse_duration_in_samples(ReadonlyBytes packet)
{
    auto cursor = make_ref_counted<ReadonlyBytesCursor>(packet);
    return parse_duration_in_samples(*cursor, packet.size());
}

DecoderErrorOr<AK::Duration> AudioPacketDurationParser::parse_duration(MediaStreamCursor& cursor, size_t packet_size)
{
    return AK::Duration::from_time_units(TRY(parse_duration_in_samples(cursor, packet_size)), 1, samples_per_second());
}

void AudioPacketDurationParser::reset()
{
    m_implementation.visit(
        [](Empty) { },
        [](auto& implementation) { implementation.reset(); });
}

}
