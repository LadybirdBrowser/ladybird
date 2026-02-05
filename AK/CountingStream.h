/*
 * Copyright (c) 2023, Tim Schumacher <timschumi@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MaybeOwned.h>
#include <AK/Stream.h>

namespace AK {

class CountingStream : public Stream {
public:
    CountingStream(MaybeOwned<Stream>, size_t offset = 0);

    u64 read_bytes() const;

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<void> discard(size_t discarded_bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;

private:
    MaybeOwned<Stream> m_stream;
    u64 m_read_bytes { 0 };
};

}
