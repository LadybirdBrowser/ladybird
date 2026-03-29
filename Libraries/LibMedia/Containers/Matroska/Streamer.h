/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>

namespace Media::Matroska {

class MEDIA_API Streamer {
public:
    Streamer(NonnullRefPtr<MediaStreamCursor> const& stream_cursor);
    ~Streamer();

    DecoderErrorOr<u8> read_octet();

    DecoderErrorOr<i16> read_i16();

    DecoderErrorOr<u32> read_element_id();
    DecoderErrorOr<Optional<size_t>> read_element_size();
    DecoderErrorOr<u64> read_variable_size_integer();
    DecoderErrorOr<i64> read_variable_size_signed_integer();

    DecoderErrorOr<u64> read_u64();
    DecoderErrorOr<i64> read_i64();
    DecoderErrorOr<double> read_float();

    DecoderErrorOr<String> read_string();

    DecoderErrorOr<void> read_unknown_element();

    DecoderErrorOr<ByteBuffer> read_raw_octets(size_t num_octets);

    size_t position() const;

    DecoderErrorOr<void> seek_to_position(size_t position);

private:
    NonnullRefPtr<MediaStreamCursor> m_stream_cursor;
};

}
