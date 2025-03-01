/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/MaybeOwned.h>
#include <AK/MemoryStream.h>
#include <LibCompress/Zlib.h>
#include <LibTest/TestCase.h>

TEST_CASE(zlib_decompress_simple)
{
    Array<u8, 40> const compressed {
        0x78, 0x01, 0x01, 0x1D, 0x00, 0xE2, 0xFF, 0x54, 0x68, 0x69, 0x73, 0x20,
        0x69, 0x73, 0x20, 0x61, 0x20, 0x73, 0x69, 0x6D, 0x70, 0x6C, 0x65, 0x20,
        0x74, 0x65, 0x78, 0x74, 0x20, 0x66, 0x69, 0x6C, 0x65, 0x20, 0x3A, 0x29,
        0x99, 0x5E, 0x09, 0xE8
    };

    u8 const uncompressed[] = "This is a simple text file :)";

    auto decompressed = TRY_OR_FAIL(Compress::ZlibDecompressor::decompress_all(compressed));
    EXPECT(decompressed.bytes() == (ReadonlyBytes { uncompressed, sizeof(uncompressed) - 1 }));
}

TEST_CASE(zlib_decompress_stream)
{
    Array<u8, 40> const compressed {
        0x78, 0x01, 0x01, 0x1D, 0x00, 0xE2, 0xFF, 0x54, 0x68, 0x69, 0x73, 0x20,
        0x69, 0x73, 0x20, 0x61, 0x20, 0x73, 0x69, 0x6D, 0x70, 0x6C, 0x65, 0x20,
        0x74, 0x65, 0x78, 0x74, 0x20, 0x66, 0x69, 0x6C, 0x65, 0x20, 0x3A, 0x29,
        0x99, 0x5E, 0x09, 0xE8
    };

    u8 const uncompressed[] = "This is a simple text file :)";

    auto stream = make<AllocatingMemoryStream>();
    auto input = MaybeOwned<Stream> { *stream };
    auto decompressor = TRY_OR_FAIL(Compress::ZlibDecompressor::create(move(input)));
    TRY_OR_FAIL(stream->write_until_depleted(compressed));
    auto decompressed = TRY_OR_FAIL(decompressor->read_until_eof());
    EXPECT(decompressed.bytes() == (ReadonlyBytes { uncompressed, sizeof(uncompressed) - 1 }));
}

TEST_CASE(zlib_round_trip_simple_default)
{
    u8 const uncompressed[] = "This is a simple text file :)";

    auto const freshly_pressed = TRY_OR_FAIL(Compress::ZlibCompressor::compress_all({ uncompressed, sizeof(uncompressed) - 1 }, Compress::GenericZlibCompressionLevel::Default));
    EXPECT(freshly_pressed.span().slice(0, 2) == ReadonlyBytes { { 0x78, 0x9C } });

    auto const decompressed = TRY_OR_FAIL(Compress::ZlibDecompressor::decompress_all(freshly_pressed));
    EXPECT(decompressed.bytes() == (ReadonlyBytes { uncompressed, sizeof(uncompressed) - 1 }));
}

TEST_CASE(zlib_round_trip_simple_best)
{
    u8 const uncompressed[] = "This is a simple text file :)";

    auto const freshly_pressed = TRY_OR_FAIL(Compress::ZlibCompressor::compress_all({ uncompressed, sizeof(uncompressed) - 1 }, Compress::GenericZlibCompressionLevel::Best));
    EXPECT(freshly_pressed.span().slice(0, 2) == ReadonlyBytes { { 0x78, 0xDA } });

    auto const decompressed = TRY_OR_FAIL(Compress::ZlibDecompressor::decompress_all(freshly_pressed));
    EXPECT(decompressed.bytes() == (ReadonlyBytes { uncompressed, sizeof(uncompressed) - 1 }));
}

TEST_CASE(zlib_round_trip_simple_fastest)
{
    u8 const uncompressed[] = "This is a simple text file :)";

    auto const freshly_pressed = TRY_OR_FAIL(Compress::ZlibCompressor::compress_all({ uncompressed, sizeof(uncompressed) - 1 }, Compress::GenericZlibCompressionLevel::Fastest));
    EXPECT(freshly_pressed.span().slice(0, 2) == ReadonlyBytes { { 0x78, 0x01 } });

    auto const decompressed = TRY_OR_FAIL(Compress::ZlibDecompressor::decompress_all(freshly_pressed));
    EXPECT(decompressed.bytes() == (ReadonlyBytes { uncompressed, sizeof(uncompressed) - 1 }));
}

TEST_CASE(zlib_decompress_with_missing_end_bits)
{
    // This test case has been extracted from compressed PNG data of `/res/icons/16x16/app-masterword.png`.
    // The decompression results have been confirmed using the `zlib-flate` tool.

    Array<u8, 72> const compressed {
        0x08, 0xD7, 0x63, 0x30, 0x86, 0x00, 0x01, 0x06, 0x23, 0x25, 0x30, 0x00,
        0x32, 0x42, 0x95, 0x54, 0x83, 0xD0, 0x18, 0x41, 0xA1, 0x50, 0x46, 0x28,
        0x8C, 0xA1, 0x8A, 0xA1, 0x46, 0xC5, 0x35, 0x48, 0xC9, 0x05, 0x99, 0xA1,
        0xA4, 0xE2, 0x02, 0x44, 0x60, 0x93, 0x5D, 0x54, 0x54, 0x9C, 0x20, 0x0C,
        0x17, 0x17, 0x08, 0x43, 0xC5, 0xC9, 0x05, 0xA8, 0x4B, 0x50, 0x50, 0x50,
        0xC4, 0xD1, 0x45, 0x50, 0x80, 0x01, 0x06, 0x00, 0xB6, 0x1F, 0x15, 0xEF
    };
    Array<u8, 144> const uncompressed {
        0x00, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x10, 0x00, 0x32, 0x22,
        0x22, 0x22, 0x22, 0x22, 0x22, 0x10, 0x00, 0x32, 0x55, 0x22, 0x25, 0x52,
        0x22, 0x22, 0x10, 0x00, 0x32, 0x55, 0x22, 0x25, 0x52, 0x22, 0x22, 0x10,
        0x00, 0x32, 0x55, 0x52, 0x55, 0x52, 0x22, 0x22, 0x10, 0x00, 0x32, 0x55,
        0x55, 0x55, 0x52, 0x22, 0x22, 0x10, 0x00, 0x32, 0x55, 0x25, 0x25, 0x52,
        0x22, 0x22, 0x10, 0x00, 0x32, 0x55, 0x22, 0x25, 0x52, 0x22, 0x22, 0x10,
        0x00, 0x32, 0x55, 0x24, 0x45, 0x52, 0x22, 0x44, 0x10, 0x00, 0x32, 0x55,
        0x24, 0x45, 0x52, 0x22, 0x44, 0x10, 0x00, 0x32, 0x22, 0x24, 0x44, 0x22,
        0x24, 0x44, 0x10, 0x00, 0x32, 0x22, 0x22, 0x44, 0x24, 0x24, 0x42, 0x10,
        0x00, 0x32, 0x22, 0x22, 0x44, 0x44, 0x44, 0x42, 0x10, 0x00, 0x32, 0x22,
        0x22, 0x24, 0x42, 0x44, 0x22, 0x10, 0x00, 0x11, 0x11, 0x11, 0x14, 0x41,
        0x44, 0x11, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    auto decompressed = TRY_OR_FAIL(Compress::ZlibDecompressor::decompress_all(compressed));
    EXPECT_EQ(decompressed.span(), uncompressed.span());
}
