/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BitStream.h>
#include <AK/MemoryStream.h>
#include <AK/Random.h>
#include <LibCompress/Deflate.h>
#include <LibTest/TestCase.h>

TEST_CASE(canonical_code_simple)
{
    Array<u8, 32> const code {
        0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
        0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
        0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05
    };
    Array<u8, 6> const input {
        0x00, 0x42, 0x84, 0xa9, 0xb0, 0x15
    };
    Array<u32, 9> const output {
        0x00, 0x01, 0x01, 0x02, 0x03, 0x05, 0x08, 0x0d, 0x15
    };

    auto const huffman = TRY_OR_FAIL(Compress::CanonicalCode::from_bytes(code));
    auto memory_stream = MUST(try_make<FixedMemoryStream>(input));
    LittleEndianInputBitStream bit_stream { move(memory_stream) };

    for (size_t idx = 0; idx < 9; ++idx)
        EXPECT_EQ(MUST(huffman.read_symbol(bit_stream)), output[idx]);
}

TEST_CASE(canonical_code_complex)
{
    Array<u8, 6> const code {
        0x03, 0x02, 0x03, 0x03, 0x02, 0x03
    };
    Array<u8, 4> const input {
        0xa1, 0xf3, 0xa1, 0xf3
    };
    Array<u32, 12> const output {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05
    };

    auto const huffman = TRY_OR_FAIL(Compress::CanonicalCode::from_bytes(code));
    auto memory_stream = MUST(try_make<FixedMemoryStream>(input));
    LittleEndianInputBitStream bit_stream { move(memory_stream) };

    for (size_t idx = 0; idx < 12; ++idx)
        EXPECT_EQ(MUST(huffman.read_symbol(bit_stream)), output[idx]);
}

TEST_CASE(invalid_canonical_code)
{
    Array<u8, 257> code;
    code.fill(0x08);
    EXPECT(Compress::CanonicalCode::from_bytes(code).is_error());
}

TEST_CASE(deflate_decompress_compressed_block)
{
    Array<u8, 28> const compressed {
        0x0B, 0xC9, 0xC8, 0x2C, 0x56, 0x00, 0xA2, 0x44, 0x85, 0xE2, 0xCC, 0xDC,
        0x82, 0x9C, 0x54, 0x85, 0x92, 0xD4, 0x8A, 0x12, 0x85, 0xB4, 0x4C, 0x20,
        0xCB, 0x4A, 0x13, 0x00
    };

    u8 const uncompressed[] = "This is a simple text file :)";

    auto const decompressed = TRY_OR_FAIL(Compress::DeflateDecompressor::decompress_all(compressed));
    EXPECT(decompressed.bytes() == ReadonlyBytes({ uncompressed, sizeof(uncompressed) - 1 }));
}

TEST_CASE(deflate_decompress_uncompressed_block)
{
    Array<u8, 18> const compressed {
        0x01, 0x0d, 0x00, 0xf2, 0xff, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x2c, 0x20,
        0x57, 0x6f, 0x72, 0x6c, 0x64, 0x21
    };

    u8 const uncompressed[] = "Hello, World!";

    auto const decompressed = TRY_OR_FAIL(Compress::DeflateDecompressor::decompress_all(compressed));
    EXPECT(decompressed.bytes() == (ReadonlyBytes { uncompressed, sizeof(uncompressed) - 1 }));
}

TEST_CASE(deflate_decompress_multiple_blocks)
{
    Array<u8, 72> const compressed {
        0x00, 0x1f, 0x00, 0xe0, 0xff, 0x54, 0x68, 0x65, 0x20, 0x66, 0x69, 0x72,
        0x73, 0x74, 0x20, 0x62, 0x6c, 0x6f, 0x63, 0x6b, 0x20, 0x69, 0x73, 0x20,
        0x75, 0x6e, 0x63, 0x6f, 0x6d, 0x70, 0x72, 0x65, 0x73, 0x73, 0x65, 0x64,
        0x53, 0x48, 0xcc, 0x4b, 0x51, 0x28, 0xc9, 0x48, 0x55, 0x28, 0x4e, 0x4d,
        0xce, 0x07, 0x32, 0x93, 0x72, 0xf2, 0x93, 0xb3, 0x15, 0x32, 0x8b, 0x15,
        0x92, 0xf3, 0x73, 0x0b, 0x8a, 0x52, 0x8b, 0x8b, 0x53, 0x53, 0xf4, 0x00
    };

    u8 const uncompressed[] = "The first block is uncompressed and the second block is compressed.";

    auto const decompressed = TRY_OR_FAIL(Compress::DeflateDecompressor::decompress_all(compressed));
    EXPECT(decompressed.bytes() == (ReadonlyBytes { uncompressed, sizeof(uncompressed) - 1 }));
}

TEST_CASE(deflate_decompress_zeroes)
{
    Array<u8, 20> const compressed {
        0xed, 0xc1, 0x01, 0x0d, 0x00, 0x00, 0x00, 0xc2, 0xa0, 0xf7, 0x4f, 0x6d,
        0x0f, 0x07, 0x14, 0x00, 0x00, 0x00, 0xf0, 0x6e
    };

    Array<u8, 4096> const uncompressed { 0 };

    auto const decompressed = TRY_OR_FAIL(Compress::DeflateDecompressor::decompress_all(compressed));
    EXPECT(uncompressed == decompressed.bytes());
}

TEST_CASE(deflate_round_trip_store)
{
    auto original = ByteBuffer::create_uninitialized(1024).release_value();
    fill_with_random(original);
    auto compressed = TRY_OR_FAIL(Compress::DeflateCompressor::compress_all(original));
    auto uncompressed = TRY_OR_FAIL(Compress::DeflateDecompressor::decompress_all(compressed));
    EXPECT(uncompressed == original);
}

TEST_CASE(deflate_round_trip_compress)
{
    auto original = ByteBuffer::create_zeroed(2048).release_value();
    fill_with_random(original.bytes().trim(1024)); // we pre-filled the second half with 0s to make sure we test back references as well
    // Since the different levels just change how much time is spent looking for better matches, just use fast here to reduce test time
    auto compressed = TRY_OR_FAIL(Compress::DeflateCompressor::compress_all(original));
    auto uncompressed = TRY_OR_FAIL(Compress::DeflateDecompressor::decompress_all(compressed));
    EXPECT(uncompressed == original);
}

TEST_CASE(deflate_round_trip_compress_large)
{
    // Compress a buffer larger than the maximum block size to test the sliding window mechanism
    auto original = TRY_OR_FAIL(ByteBuffer::create_uninitialized((32 * KiB - 1) * 2));
    fill_with_random(original);

    // Since the different levels just change how much time is spent looking for better matches, just use fast here to reduce test time
    auto compressed = TRY_OR_FAIL(Compress::DeflateCompressor::compress_all(original));
    auto uncompressed = TRY_OR_FAIL(Compress::DeflateDecompressor::decompress_all(compressed));
    EXPECT(uncompressed.span() == original.span().slice(0, uncompressed.size()));
    EXPECT(uncompressed == original);
}

TEST_CASE(deflate_compress_literals)
{
    // This byte array is known to not produce any back references with our lz77 implementation even at the highest compression settings
    Array<u8, 0x13> test { 0, 0, 0, 0, 0x72, 0, 0, 0xee, 0, 0, 0, 0x26, 0, 0, 0, 0x28, 0, 0, 0x72 };
    auto compressed = TRY_OR_FAIL(Compress::DeflateCompressor::compress_all(test));
}
