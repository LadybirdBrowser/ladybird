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
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaStream.h>

namespace Media::ISOBMFF {

class MEDIA_API Streamer {
public:
    Streamer(NonnullRefPtr<MediaStreamCursor> const& stream_cursor);
    ~Streamer();

    template<Integral T>
    DecoderErrorOr<T> read()
    {
        return m_stream_cursor->read_value<T>();
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
