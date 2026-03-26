/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/MediaStream.h>

namespace Media {

class ReadonlyBytesCursor final : public MediaStreamCursor {
public:
    ReadonlyBytesCursor(ReadonlyBytes data)
        : m_data(data)
    {
    }

    virtual DecoderErrorOr<void> seek(i64 offset, SeekMode mode) override
    {
        auto target_position = [&] -> size_t {
            switch (mode) {
            case SeekMode::SetPosition:
                return offset;
            case SeekMode::FromCurrentPosition:
                return m_position + offset;
            case SeekMode::FromEndPosition:
                return m_data.size() + offset;
            }
            VERIFY_NOT_REACHED();
        }();
        if (target_position > m_data.size())
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of buffer"sv);

        m_position = target_position;
        return {};
    }

    virtual DecoderErrorOr<size_t> read_into(Bytes bytes) override
    {
        if (m_position >= m_data.size())
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of buffer"sv);

        auto available = m_data.size() - m_position;
        if (available < bytes.size())
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of buffer"sv);

        auto to_read = bytes.size();
        m_data.slice(m_position, to_read).copy_to(bytes);
        m_position += to_read;
        VERIFY(m_position <= m_data.size());
        return to_read;
    }

    virtual size_t position() const override { return m_position; }
    virtual size_t size() const override { return m_data.size(); }

    void set_data(ReadonlyBytes data) { m_data = data; }

private:
    ReadonlyBytes m_data;
    size_t m_position { 0 };
};

}
