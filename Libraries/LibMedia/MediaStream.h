/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Endian.h>
#include <AK/FixedArray.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Stream.h>
#include <AK/Vector.h>
#include <LibMedia/DecoderError.h>

namespace Media {

enum class ReadBlocked : u8 {
    No,
    Yes,
};

using ReadBlockedChangeHandler = Function<void(ReadBlocked)>;

class MediaStreamCursor : public AtomicRefCounted<MediaStreamCursor> {
public:
    virtual ~MediaStreamCursor() = default;

    virtual void set_is_blocking(bool) = 0;

    virtual DecoderErrorOr<void> seek(i64 offset, AK::SeekMode) = 0;
    virtual DecoderErrorOr<size_t> read_into(Bytes) = 0;
    virtual DecoderErrorOr<FixedArray<u8>> read_bytes(size_t size) = 0;
    virtual size_t position() const = 0;
    virtual size_t size() const = 0;

    DecoderErrorOr<void> read_until_filled(Bytes buffer)
    {
        auto bytes_read = TRY(read_into(buffer));
        if (bytes_read != buffer.size())
            return DecoderError::corrupted("Unexpected end of stream"sv);
        return {};
    }

    template<Integral T>
    DecoderErrorOr<T> read_value(AK::Endianness endianness = AK::Endianness::Big)
    {
        T value = 0;
        TRY(read_until_filled({ &value, sizeof(value) }));
        switch (endianness) {
        case AK::Endianness::Host:
            return value;
        case AK::Endianness::Big:
            return AK::convert_between_host_and_big_endian(value);
        case AK::Endianness::Little:
            return AK::convert_between_host_and_little_endian(value);
        }
        VERIFY_NOT_REACHED();
    }

    DecoderErrorOr<void> seek_to_position(size_t position)
    {
        if (position > NumericLimits<i64>::max())
            return DecoderError::corrupted("Seek position is too large"sv);
        return seek(static_cast<i64>(position), AK::SeekMode::SetPosition);
    }

    DecoderErrorOr<void> skip(i64 bytes)
    {
        return seek(bytes, AK::SeekMode::FromCurrentPosition);
    }

    virtual void abort() { }
    virtual void reset_abort() { }
    virtual bool is_aborted() const { return false; }

    // The handler may be invoked while the stream's lock is held, so it must not call back into the stream.
    virtual void set_blocked_change_handler(ReadBlockedChangeHandler) { }
};

class MediaStream : public AtomicRefCounted<MediaStream> {
public:
    struct ByteRange {
        size_t start { 0 };
        size_t end { 0 };
    };

    virtual ~MediaStream() = default;

    virtual NonnullRefPtr<MediaStreamCursor> create_cursor() = 0;

    virtual Vector<ByteRange> available_byte_ranges() const = 0;

    virtual Optional<u64> expected_size() const = 0;

    virtual bool is_closed() const = 0;

    bool closing_bytes_are_available() const
    {
        if (!is_closed())
            return false;
        auto ranges = available_byte_ranges();
        return !ranges.is_empty() && ranges.last().end >= expected_size().value();
    }

    // The observer is invoked under the stream's lock, so it must be cheap and must not call back into the stream.
    virtual void set_available_ranges_change_observer(Function<void()>) { }
};

}
