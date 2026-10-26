/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BitStream.h>
#include <AK/MemoryStream.h>
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
