/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Random.h>
#include <LibCompress/Brotli.h>
#include <LibTest/TestCase.h>

TEST_CASE(brotli_decompress_simple)
{
    Array<u8, 20> const compressed {
        0x21, 0x38, 0x00, 0x04, 0x65, 0x78, 0x70, 0x65, 0x63, 0x74,
        0x65, 0x64, 0x20, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x03
    };

    u8 const uncompressed[] = "expected output";

    auto const decompressed = TRY_OR_FAIL(Compress::BrotliDecompressor::decompress_all(compressed));
    EXPECT(decompressed.bytes() == (ReadonlyBytes { uncompressed, sizeof(uncompressed) - 1 }));
}

TEST_CASE(brotli_decompress_empty)
{
    Array<u8, 2> const compressed { 0xa1, 0x01 };

    auto const decompressed = TRY_OR_FAIL(Compress::BrotliDecompressor::decompress_all(compressed));
    EXPECT(decompressed.is_empty());
}

TEST_CASE(brotli_round_trip)
{
    auto original = ByteBuffer::create_uninitialized(1024).release_value();
    fill_with_random(original);

    auto compressed = TRY_OR_FAIL(Compress::BrotliCompressor::compress_all(original));
    auto uncompressed = TRY_OR_FAIL(Compress::BrotliDecompressor::decompress_all(compressed));
    EXPECT(uncompressed == original);
}

TEST_CASE(brotli_compression_levels)
{
    Array compression_levels {
        Compress::BrotliCompressionLevel::Fastest,
        Compress::BrotliCompressionLevel::Default,
        Compress::BrotliCompressionLevel::Best,
    };

    auto original = "Ladybird Brotli compression level test data"sv.bytes();
    for (auto compression_level : compression_levels) {
        auto compressed = TRY_OR_FAIL(Compress::BrotliCompressor::compress_all(original, compression_level));
        auto uncompressed = TRY_OR_FAIL(Compress::BrotliDecompressor::decompress_all(compressed));
        EXPECT(uncompressed == original);
    }
}

TEST_CASE(brotli_trailing_data)
{
    Array<u8, 21> const compressed {
        0x21, 0x38, 0x00, 0x04, 0x65, 0x78, 0x70, 0x65, 0x63, 0x74,
        0x65, 0x64, 0x20, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x03,
        0x00
    };

    auto const decompressed = Compress::BrotliDecompressor::decompress_all(compressed);
    EXPECT(decompressed.is_error());
}

TEST_CASE(brotli_trailing_data_after_end)
{
    Array<u8, 20> const compressed {
        0x21, 0x38, 0x00, 0x04, 0x65, 0x78, 0x70, 0x65, 0x63, 0x74,
        0x65, 0x64, 0x20, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x03
    };

    auto input_stream = make<AllocatingMemoryStream>();
    auto decompressor = TRY_OR_FAIL(Compress::BrotliDecompressor::create(MaybeOwned<Stream> { *input_stream }));

    TRY_OR_FAIL(input_stream->write_until_depleted(compressed));

    Array<u8, 32> output;
    auto const decompressed = TRY_OR_FAIL(decompressor->read_some(output));
    u8 const uncompressed[] = "expected output";
    EXPECT(decompressed == (ReadonlyBytes { uncompressed, sizeof(uncompressed) - 1 }));

    u8 trailing_data = 0;
    TRY_OR_FAIL(input_stream->write_until_depleted({ &trailing_data, 1 }));
    EXPECT(decompressor->read_some(output).is_error());
}
