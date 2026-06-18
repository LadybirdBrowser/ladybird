/*
 * Copyright (c) 2021, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/LEB128.h>
#include <AK/MemoryStream.h>
#include <AK/NumericLimits.h>
#include <LibTest/TestCase.h>

TEST_CASE(single_byte)
{
    u32 output = {};
    i32 output_signed = {};
    u8 buf[] = { 0x00 };
    FixedMemoryStream stream { ReadonlyBytes { buf, sizeof(buf) } };

    // less than/eq 0b0011_1111, signed == unsigned == raw byte
    for (u8 i = 0u; i <= 0x3F; ++i) {
        buf[0] = i;

        MUST(stream.seek(0));
        output = MUST(stream.read_value<LEB128<u32>>());
        EXPECT_EQ(output, i);

        MUST(stream.seek(0));
        output_signed = MUST(stream.read_value<LEB128<i32>>());
        EXPECT_EQ(output_signed, i);
    }

    // 0b0100_0000 to 0b0111_1111 unsigned == byte, signed = {{ 26'b(-1), 6'b(byte) }}
    for (u8 i = 0x40u; i < 0x80; ++i) {
        buf[0] = i;

        MUST(stream.seek(0));
        output = MUST(stream.read_value<LEB128<u32>>());
        EXPECT_EQ(output, i);

        MUST(stream.seek(0));
        output_signed = MUST(stream.read_value<LEB128<i32>>());
        EXPECT_EQ(output_signed, (i | (-1 & (~0x3F))));
    }
    // MSB set, but input too short
    for (u16 i = 0x80; i <= 0xFF; ++i) {
        buf[0] = static_cast<u8>(i);

        MUST(stream.seek(0));
        EXPECT(stream.read_value<LEB128<u32>>().is_error());

        MUST(stream.seek(0));
        EXPECT(stream.read_value<LEB128<i32>>().is_error());
    }
}

TEST_CASE(two_bytes)
{
    u32 output = {};
    i32 output_signed = {};
    u8 buf[] = { 0x00, 0x1 };
    FixedMemoryStream stream { ReadonlyBytes { buf, sizeof(buf) } };

    // Only test with first byte expecting more, otherwise equivalent to single byte case
    for (u16 i = 0x80; i <= 0xFF; ++i) {
        buf[0] = static_cast<u8>(i);

        // less than/eq 0b0011_1111: signed == unsigned == (j << 7) + (7 MSB of i)
        for (u8 j = 0u; j <= 0x3F; ++j) {
            buf[1] = j;

            MUST(stream.seek(0));
            output = MUST(stream.read_value<LEB128<u32>>());
            EXPECT_EQ(output, (static_cast<u32>(j) << 7) + (i & 0x7F));

            MUST(stream.seek(0));
            output_signed = MUST(stream.read_value<LEB128<i32>>());
            EXPECT_EQ(output_signed, (static_cast<i32>(j) << 7) + (i & 0x7F));
        }

        // 0b0100_0000 to 0b0111_1111: unsigned == (j << 7) + (7 MSB of i), signed == {{ 19'b(-1), 6'b(j), 7'b(i) }}
        for (u8 j = 0x40u; j < 0x80; ++j) {
            buf[1] = j;

            MUST(stream.seek(0));
            output = MUST(stream.read_value<LEB128<u32>>());
            EXPECT_EQ(output, (static_cast<u32>(j) << 7) + (i & 0x7F));

            MUST(stream.seek(0));
            output_signed = MUST(stream.read_value<LEB128<i32>>());
            EXPECT_EQ(output_signed, ((static_cast<i32>(j) << 7) + (i & 0x7F)) | (-1 & (~0x3FFF)));
        }

        // MSB set on last byte, but input too short
        for (u16 j = 0x80; j <= 0xFF; ++j) {
            buf[1] = static_cast<u8>(j);

            MUST(stream.seek(0));
            EXPECT(stream.read_value<LEB128<u32>>().is_error());

            MUST(stream.seek(0));
            EXPECT(stream.read_value<LEB128<i32>>().is_error());
        }
    }
}

TEST_CASE(overflow_sizeof_output_unsigned)
{
    u8 u32_max_plus_one[] = { 0x80, 0x80, 0x80, 0x80, 0x10 };
    {
        FixedMemoryStream stream { ReadonlyBytes { u32_max_plus_one, sizeof(u32_max_plus_one) } };
        EXPECT(stream.read_value<LEB128<u32>>().is_error());

        MUST(stream.seek(0));
        u64 out64 = MUST(stream.read_value<LEB128<u64>>());
        EXPECT_EQ(out64, static_cast<u64>(NumericLimits<u32>::max()) + 1);
    }

    u8 u32_max[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F };
    {
        FixedMemoryStream stream { ReadonlyBytes { u32_max, sizeof(u32_max) } };
        u32 out = MUST(stream.read_value<LEB128<u32>>());
        EXPECT_EQ(out, NumericLimits<u32>::max());

        MUST(stream.seek(0));
        u64 out64 = MUST(stream.read_value<LEB128<u64>>());
        EXPECT_EQ(out64, NumericLimits<u32>::max());
    }
}

TEST_CASE(overflow_sizeof_output_signed)
{
    u8 i32_max_plus_one[] = { 0x80, 0x80, 0x80, 0x80, 0x08 };
    {
        FixedMemoryStream stream { ReadonlyBytes { i32_max_plus_one, sizeof(i32_max_plus_one) } };
        EXPECT(stream.read_value<LEB128<i32>>().is_error());

        MUST(stream.seek(0));
        i64 out64 = MUST(stream.read_value<LEB128<i64>>());
        EXPECT_EQ(out64, static_cast<i64>(NumericLimits<i32>::max()) + 1);
    }

    u8 i32_max[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x07 };
    {
        FixedMemoryStream stream { ReadonlyBytes { i32_max, sizeof(i32_max) } };
        i32 out = MUST(stream.read_value<LEB128<i32>>());
        EXPECT_EQ(out, NumericLimits<i32>::max());

        MUST(stream.seek(0));
        i64 out64 = MUST(stream.read_value<LEB128<i64>>());
        EXPECT_EQ(out64, NumericLimits<i32>::max());
    }

    u8 i32_min_minus_one[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x77 };
    {
        FixedMemoryStream stream { ReadonlyBytes { i32_min_minus_one, sizeof(i32_min_minus_one) } };
        EXPECT(stream.read_value<LEB128<i32>>().is_error());

        MUST(stream.seek(0));
        i64 out64 = MUST(stream.read_value<LEB128<i64>>());
        EXPECT_EQ(out64, static_cast<i64>(NumericLimits<i32>::min()) - 1);
    }

    u8 i32_min[] = { 0x80, 0x80, 0x80, 0x80, 0x78 };
    {
        FixedMemoryStream stream { ReadonlyBytes { i32_min, sizeof(i32_min) } };
        i32 out = MUST(stream.read_value<LEB128<i32>>());
        EXPECT_EQ(out, NumericLimits<i32>::min());

        MUST(stream.seek(0));
        i64 out64 = MUST(stream.read_value<LEB128<i64>>());
        EXPECT_EQ(out64, NumericLimits<i32>::min());
    }
}

TEST_CASE(encode_round_trips)
{
    auto round_trip_unsigned = [](u64 value) {
        AllocatingMemoryStream stream;
        MUST(stream.write_value(LEB128<u64> { value }));
        u64 decoded = MUST(stream.read_value<LEB128<u64>>());
        EXPECT_EQ(decoded, value);
    };
    Array<u64, 10> unsigned_values { 0, 1, 0x7F, 0x80, 0xFF, 0x100, 300, 0x3FFF, 0x4000, NumericLimits<u64>::max() };
    for (auto value : unsigned_values)
        round_trip_unsigned(value);

    auto round_trip_signed = [](i64 value) {
        AllocatingMemoryStream stream;
        MUST(stream.write_value(LEB128<i64> { value }));
        i64 decoded = MUST(stream.read_value<LEB128<i64>>());
        EXPECT_EQ(decoded, value);
    };
    Array<i64, 11> signed_values { 0, 1, -1, 63, 64, -64, -65, 1000, -1000, NumericLimits<i64>::min(), NumericLimits<i64>::max() };
    for (auto value : signed_values)
        round_trip_signed(value);
}

TEST_CASE(encode_unsigned_produces_canonical_bytes)
{
    auto expect_bytes = [](u64 value, ReadonlyBytes expected) {
        AllocatingMemoryStream stream;
        MUST(stream.write_value(LEB128<u64> { value }));
        auto buffer = MUST(ByteBuffer::create_uninitialized(expected.size()));
        MUST(stream.read_until_filled(buffer));
        EXPECT_EQ(buffer.bytes(), expected);
        EXPECT(stream.is_eof()); // Exactly expected.size() bytes were written.
    };

    Array<u8, 1> zero { 0x00 };
    expect_bytes(0, zero);
    Array<u8, 2> max_single_byte_value_plus_one { 0xFF, 0x01 };
    expect_bytes(0xFF, max_single_byte_value_plus_one);
    Array<u8, 2> three_hundred { 0xAC, 0x02 };
    expect_bytes(300, three_hundred);
}
