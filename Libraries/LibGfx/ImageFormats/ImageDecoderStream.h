/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/RedBlackTree.h>
#include <AK/Stream.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace Gfx {

class ImageDecoderStream
    : public SeekableStream
    , public AtomicRefCounted<ImageDecoderStream> {

public:
    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual ErrorOr<size_t> seek(i64 offset, SeekMode seek_mode) override;
    virtual ErrorOr<void> truncate(size_t length) override;

    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;

    void append_chunk(ByteBuffer&& chunk);

private:
    mutable Threading::Mutex m_mutex;
    RedBlackTree<size_t, ByteBuffer> m_chunks;
    size_t m_chunk_index { 0 };
    size_t m_offset_inside_chunk { 0 };
    bool m_closed { false };

    Threading::ConditionVariable m_waiting_for_more_data { m_mutex };
};

}
