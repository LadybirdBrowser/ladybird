/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Schumacher <timschumi@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MaybeOwned.h>
#include <AK/Stream.h>

namespace AK {

class ConstrainedStream : public Stream {
public:
    ConstrainedStream(MaybeOwned<Stream>, u64 limit);

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<void> discard(size_t discarded_bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;
    u64 remaining() const { return m_limit; }

private:
    MaybeOwned<Stream> m_stream;
    u64 m_limit;
};

}
