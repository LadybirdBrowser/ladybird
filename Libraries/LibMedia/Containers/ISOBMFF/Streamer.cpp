/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Streamer.h"

namespace Media::ISOBMFF {

Streamer::Streamer(NonnullRefPtr<MediaStreamCursor> const& stream_cursor)
    : m_stream_cursor(stream_cursor)
{
}

Streamer::~Streamer() = default;

DecoderErrorOr<FourCC> Streamer::read_four_cc()
{
    return FourCC { TRY(read<u32>()) };
}

DecoderErrorOr<double> Streamer::read_fixed_point_16_16()
{
    return static_cast<double>(TRY(read<u32>())) / 65536.0;
}

DecoderErrorOr<String> Streamer::read_null_terminated_string(size_t maximum_size)
{
    auto bytes = TRY(read_bytes(maximum_size));
    auto length = maximum_size;
    for (size_t i = 0; i < maximum_size; i++) {
        if (bytes[i] == 0) {
            length = i;
            break;
        }
    }
    auto string = String::from_utf8(StringView { bytes.data(), length });
    if (string.is_error())
        return DecoderError::format(DecoderErrorCategory::Invalid, "String is not valid UTF-8");
    return string.release_value();
}

DecoderErrorOr<FixedArray<u8>> Streamer::read_bytes(size_t size)
{
    return m_stream_cursor->read_bytes(size);
}

DecoderErrorOr<void> Streamer::skip(size_t size)
{
    if (size > NumericLimits<i64>::max())
        return DecoderError::corrupted("Cannot skip past the end of the stream"sv);
    return m_stream_cursor->skip(static_cast<i64>(size));
}

size_t Streamer::position() const
{
    return m_stream_cursor->position();
}

DecoderErrorOr<void> Streamer::seek_to_position(size_t position)
{
    return m_stream_cursor->seek_to_position(position);
}

}
