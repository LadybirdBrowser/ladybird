/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <LibMedia/DecoderError.h>
#include <LibThreading/Mutex.h>

namespace Media {

class IncrementallyPopulatedStream : public AtomicRefCounted<IncrementallyPopulatedStream> {
public:
    static NonnullRefPtr<IncrementallyPopulatedStream> create()
    {
        return adopt_ref(*new IncrementallyPopulatedStream({}));
    }

    static NonnullRefPtr<IncrementallyPopulatedStream> create_from_byte_buffer(ByteBuffer data)
    {
        return adopt_ref(*new IncrementallyPopulatedStream(data));
    }

    void append(ByteBuffer data)
    {
        Threading::MutexLocker locker { m_mutex };
        m_data.append(move(data));
    }

    bool complete() const { return m_complete; }
    void set_complete()
    {
        Threading::MutexLocker locker { m_mutex };
        m_complete = true;
    }

    ReadonlyBytes data() const { return m_data; }
    auto size() const { return m_data.size(); }

    DecoderErrorOr<u8> read_octet(size_t position)
    {
        Threading::MutexLocker locker { m_mutex };
        if (position >= m_data.size()) {
            if (m_complete)
                return DecoderError::corrupted("Tried to read past end of complete stream"sv);
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "Not enough data in stream"sv);
        }
        return m_data[position];
    }

    DecoderErrorOr<ByteBuffer> read_octets(size_t position, size_t count)
    {
        Threading::MutexLocker locker { m_mutex };
        if (position + count > m_data.size()) {
            if (m_complete)
                return DecoderError::corrupted("Tried to read past end of complete stream"sv);
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "Not enough data in stream"sv);
        }
        return MUST(m_data.slice(position, count));
    }

private:
    IncrementallyPopulatedStream(ByteBuffer data)
        : m_data(move(data))
    {
    }

    Threading::Mutex m_mutex;
    ByteBuffer m_data {};
    bool m_complete { false };
};

}
