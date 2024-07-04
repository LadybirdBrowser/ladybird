/*
 * Copyright (c) 2022, Michiel Visser <opensource@webmichiel.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/BitStream.h>
#include <AK/MaybeOwned.h>
#include <AK/MemoryStream.h>
#include <LibCompress/Brotli.h>
#include <LibCore/File.h>

TEST_CASE(dictionary_use_after_uncompressed_block)
{
    // This input file contains one block of uncompressed data ("WHF") and then invokes
    // a copy command that, together with the default distance, results in a dictionary
    // lookup-and-copy ("categories").
    // That in particular isn't a special combination, but dictionary indices depend on
    // the count of bytes that have been decompressed so far, and we previously had
    // a bug where uncompressed data was unaccounted for.

    auto stream = make<AllocatingMemoryStream>();

    // Brotli operates on bits instead of bytes, so we can't easily use a well-documented byte array.
    // Instead, assemble the test case on-the-fly via a bit stream.
    auto stream_in = LittleEndianOutputBitStream { MaybeOwned<Stream>(*stream) };
    MUST(stream_in.write_bits(0b0u, 1u)); // WBITS = 16

    MUST(stream_in.write_bits(0b0u, 1u));  // ISLAST = false
    MUST(stream_in.write_bits(0b00u, 2u)); // MNIBBLES = 4
    MUST(stream_in.write_bits(2u, 16u));   // MLEN - 1 = 2
    MUST(stream_in.write_bits(0b1u, 1u));  // ISUNCOMPRESSED = true
    MUST(stream_in.align_to_byte_boundary());
    MUST(stream_in.write_until_depleted("WHF"sv.bytes())); // Literal uncompressed data

    MUST(stream_in.write_bits(0b1u, 1u));    // ISLAST = true
    MUST(stream_in.write_bits(0b0u, 1u));    // ISLASTEMPTY = false
    MUST(stream_in.write_bits(0b00u, 2u));   // MNIBBLES = 4
    MUST(stream_in.write_bits(9u, 16u));     // MLEN - 1 = 9
    MUST(stream_in.write_bits(0b0u, 1u));    // NBLTYPESL = 1
    MUST(stream_in.write_bits(0b0u, 1u));    // NBLTYPESI = 1
    MUST(stream_in.write_bits(0b0u, 1u));    // NBLTYPESD = 1
    MUST(stream_in.write_bits(0b00u, 2u));   // NPOSTFIX = 0
    MUST(stream_in.write_bits(0b0000u, 4u)); // NDIRECT = 0
    MUST(stream_in.write_bits(0b10u, 2u));   // CMODE[0] = 2
    MUST(stream_in.write_bits(0b0u, 1u));    // NTREESL = 1
    MUST(stream_in.write_bits(0b0u, 1u));    // NTREESD = 1
    MUST(stream_in.write_bits(0b01u, 2u));   // literal_codes[0] hskip = 1
    MUST(stream_in.write_bits(0b00u, 2u));   // literal_codes[0] number of symbols - 1 = 0
    MUST(stream_in.write_bits(0u, 8u));      // literal_codes[0] symbols[0] = 0 (unused)
    MUST(stream_in.write_bits(0b01u, 2u));   // iac_codes[0] hskip = 1
    MUST(stream_in.write_bits(0b00u, 2u));   // iac_codes[0] number of symbols - 1 = 0
    MUST(stream_in.write_bits(64u, 10u));    // iac_codes[0] symbols[0] = 64 (index = 1, insert_offset = 0, copy_offset = 0)
    MUST(stream_in.write_bits(0b01u, 2u));   // distance_codes[0] hskip = 1
    MUST(stream_in.write_bits(0b00u, 2u));   // distance_codes[0] number of symbols - 1 = 0
    MUST(stream_in.write_bits(0u, 6u));      // distance_codes[0] symbols[0] = 0 (unused)

    MUST(stream_in.align_to_byte_boundary());
    MUST(stream_in.flush_buffer_to_stream());

    auto decompressor = Compress::BrotliDecompressionStream { MaybeOwned<Stream>(*stream) };
    auto buffer = TRY_OR_FAIL(decompressor.read_until_eof());

    EXPECT_EQ(buffer.span(), "WHFcategories"sv.bytes());
}

static void run_test(StringView const file_name)
{
    ByteString path = ByteString::formatted("brotli-test-files/{}", file_name);

    auto cmp_file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto cmp_data = MUST(cmp_file->read_until_eof());

    ByteString path_compressed = ByteString::formatted("{}.br", path);

    auto file = MUST(Core::File::open(path_compressed, Core::File::OpenMode::Read));
    auto brotli_stream = Compress::BrotliDecompressionStream { MaybeOwned<Stream> { *file } };
    auto data = MUST(brotli_stream.read_until_eof());

    EXPECT_EQ(data, cmp_data);
}

TEST_CASE(brotli_decompress_uncompressed)
{
    run_test("wellhello.txt"sv);
}

TEST_CASE(brotli_decompress_simple)
{
    run_test("hello.txt"sv);
}

TEST_CASE(brotli_decompress_simple2)
{
    run_test("wellhello2.txt"sv);
}

TEST_CASE(brotli_decompress_lorem)
{
    run_test("lorem.txt"sv);
}

TEST_CASE(brotli_decompress_lorem2)
{
    run_test("lorem2.txt"sv);
}

TEST_CASE(brotli_decompress_transform)
{
    run_test("transform.txt"sv);
}

TEST_CASE(brotli_decompress_serenityos_html)
{
    run_test("serenityos.html"sv);
}

TEST_CASE(brotli_decompress_happy3rd_html)
{
    run_test("happy3rd.html"sv);
}

TEST_CASE(brotli_single_z)
{
    run_test("single-z.txt"sv);
}

TEST_CASE(brotli_single_x)
{
    run_test("single-x.txt"sv);
}

TEST_CASE(brotli_decompress_zero_one_bin)
{
    ByteString path = "brotli-test-files/zero-one.bin";

    ByteString path_compressed = ByteString::formatted("{}.br", path);

    auto file = MUST(Core::File::open(path_compressed, Core::File::OpenMode::Read));
    auto brotli_stream = Compress::BrotliDecompressionStream { MaybeOwned<Stream> { *file } };

    u8 buffer_raw[4096];
    Bytes buffer { buffer_raw, 4096 };

    size_t bytes_read = 0;
    while (true) {
        size_t nread = MUST(brotli_stream.read_some(buffer)).size();
        if (nread == 0)
            break;

        for (size_t i = 0; i < nread; i++) {
            if (bytes_read < 16 * MiB)
                EXPECT(buffer[i] == 0);
            else
                EXPECT(buffer[i] == 1);
        }

        bytes_read += nread;
    }
    EXPECT(bytes_read == 32 * MiB);
    EXPECT(brotli_stream.is_eof());
}
