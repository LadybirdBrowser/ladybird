/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibAudio/ChannelMap.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaStream.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/DecoderError.h>

namespace Media::WAV {

struct SampleFormat {
    CodecID codec_id;
    u32 sample_rate;
    u16 bytes_per_frame;
    u32 bytes_per_second;
    Audio::ChannelMap channel_map;
};

// Reads the RIFF header of a WAVE stream up to its sample data.
class MEDIA_API Reader {
public:
    static DecoderErrorOr<Reader> from_stream(NonnullRefPtr<MediaStreamCursor> const&);

    SampleFormat const& sample_format() const { return m_sample_format; }
    size_t sample_data_offset() const { return m_sample_data_offset; }
    // Unknown when the header leaves the size open, as writers that stream do, and the stream's size is unknown too.
    Optional<u64> sample_data_end() const { return m_sample_data_end; }
    AK::Duration duration() const;

private:
    Reader(SampleFormat, size_t sample_data_offset, Optional<u64> sample_data_end);

    SampleFormat m_sample_format;
    size_t m_sample_data_offset { 0 };
    Optional<u64> m_sample_data_end;
};

}
