/*
 * Copyright (c) 2026, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/MaybeOwned.h>
#include <AK/MemoryStream.h>
#include <AK/StdLibExtras.h>
#include <AK/Stream.h>

namespace Compress {

template<class T>
ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes bytes)
{
    auto input_stream = make<AK::FixedMemoryStream>(bytes);
    auto decompressor = TRY(T::create(MaybeOwned<Stream>(move(input_stream))));
    return TRY(decompressor->read_until_eof(4096));
}

template<class T, class... Args>
ErrorOr<ByteBuffer> compress_all(ReadonlyBytes bytes, Args&&... args)
{
    auto output_stream = TRY(try_make<AllocatingMemoryStream>());
    auto compressor = TRY(T::create(MaybeOwned { *output_stream }, forward<Args>(args)...));

    TRY(compressor->write_until_depleted(bytes));
    TRY(compressor->finish());

    auto buffer = TRY(ByteBuffer::create_uninitialized(output_stream->used_buffer_size()));
    TRY(output_stream->read_until_filled(buffer.bytes()));

    return buffer;
}

}
