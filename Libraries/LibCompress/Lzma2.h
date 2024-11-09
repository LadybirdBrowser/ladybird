/*
 * Copyright (c) 2023, Tim Schumacher <timschumi@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CircularBuffer.h>
#include <AK/MaybeOwned.h>
#include <AK/Stream.h>
#include <LibCompress/Lzma.h>

namespace Compress {

// This is based on the human-language description of the LZMA2 format on the English Wikipedia.
// https://en.wikipedia.org/wiki/Lempel%E2%80%93Ziv%E2%80%93Markov_chain_algorithm#LZMA2_format

class Lzma2Decompressor : public Stream {
public:
    /// Creates a decompressor that does not require the leading byte indicating the dictionary size.
    static ErrorOr<NonnullOwnPtr<Lzma2Decompressor>> create_from_raw_stream(MaybeOwned<Stream>, u32 dictionary_size);

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;

private:
    Lzma2Decompressor(MaybeOwned<Stream>, CircularBuffer dictionary);

    MaybeOwned<Stream> m_stream;
    CircularBuffer m_dictionary;
    // Our dictionary is always initialized, but LZMA2 requires that the first chunk resets the dictionary.
    bool m_dictionary_initialized { false };
    bool m_found_end_of_stream { false };

    Optional<MaybeOwned<Stream>> m_current_chunk_stream;
    bool m_in_uncompressed_chunk { false };

    Optional<NonnullOwnPtr<LzmaDecompressor>> m_last_lzma_stream;
    Optional<LzmaDecompressorOptions> m_last_lzma_options;
};

}
