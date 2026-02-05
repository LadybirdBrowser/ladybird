/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Stream.h>
#include <LibMedia/DecoderError.h>

namespace Media {

class MediaStreamCursor : public AtomicRefCounted<MediaStreamCursor> {
public:
    virtual ~MediaStreamCursor() = default;

    virtual DecoderErrorOr<void> seek(i64 offset, SeekMode) = 0;
    virtual DecoderErrorOr<size_t> read_into(Bytes) = 0;
    virtual size_t position() const = 0;
    virtual size_t size() const = 0;

    virtual void abort() { }
    virtual void reset_abort() { }
    virtual bool is_blocked() const { return false; }
};

class MediaStream : public AtomicRefCounted<MediaStream> {
public:
    virtual ~MediaStream() = default;

    virtual NonnullRefPtr<MediaStreamCursor> create_cursor() = 0;
};

}
