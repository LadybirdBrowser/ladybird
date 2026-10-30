/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/Codecs/Opus.h>
#include <LibMedia/Codecs/Vorbis.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/DecoderError.h>

namespace Media::Codecs {

// Reads the duration of coded audio frames in decode order. Must be reset when a discontinuity occurs.
class AudioPacketDurationParser {
public:
    static MEDIA_API DecoderErrorOr<AudioPacketDurationParser> create(CodecID, ReadonlyBytes codec_initialization_data);

    AudioPacketDurationParser() = default;

    bool parses_durations() const { return !m_implementation.has<Empty>(); }

    DecoderErrorOr<u32> parse_duration_in_samples(MediaStreamCursor&, size_t packet_size);
    DecoderErrorOr<u32> parse_duration_in_samples(ReadonlyBytes packet);
    DecoderErrorOr<AK::Duration> parse_duration(MediaStreamCursor&, size_t packet_size);
    void reset();

private:
    using Implementation = Variant<Empty, Opus::PacketDurationParser, Vorbis::PacketDurationParser, FLAC::PacketDurationParser>;

    explicit AudioPacketDurationParser(Implementation implementation)
        : m_implementation(move(implementation))
    {
    }

    u32 samples_per_second() const;

    Implementation m_implementation;
};

}
