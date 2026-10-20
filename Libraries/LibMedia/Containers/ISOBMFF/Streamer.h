/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <LibMedia/Containers/ISOBMFF/BoxTypes.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaStream.h>
#include <LibMediaCodecs/DecoderError.h>

namespace Media::ISOBMFF {

class MEDIA_API Streamer {
public:
    Streamer(NonnullRefPtr<MediaStreamCursor> const& stream_cursor);
    ~Streamer();

    template<Integral T>
    DecoderErrorOr<T> read()
    {
        T value = 0;
        auto start = m_stream_cursor->position();
        auto count = TRY(m_stream_cursor->read_into({ &value, sizeof(T) }));
        if (count != sizeof(T)) {
            auto size = m_stream_cursor->size();
            if (size.has_value() && (start > size.value() || sizeof(T) > size.value() - start))
                return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "Unexpected end of ISO BMFF stream"sv);
            // Non-blocking cursors return short reads when a range has not arrived (or was evicted).
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "ISO BMFF data is not available yet"sv);
        }
        return AK::convert_between_host_and_big_endian(value);
    }

    DecoderErrorOr<FourCC> read_four_cc();

    DecoderErrorOr<double> read_fixed_point_16_16();

    DecoderErrorOr<String> read_null_terminated_string(size_t maximum_size);

    DecoderErrorOr<FixedArray<u8>> read_bytes(size_t size);

    DecoderErrorOr<void> skip(size_t size);

    size_t position() const;

    DecoderErrorOr<void> seek_to_position(size_t position);

    MediaStreamCursor& cursor() { return *m_stream_cursor; }

private:
    NonnullRefPtr<MediaStreamCursor> m_stream_cursor;
};

}
