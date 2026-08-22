/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/BitReader.h>
#include <LibTest/TestCase.h>

TEST_CASE(reads_fields_across_byte_boundaries)
{
    // The bit stream reads 101 1001001 00111110000001.
    Array<u8, 3> data { 0b1011'0010, 0b0100'1111, 0b1000'0001 };
    Media::BitReader reader { data };

    EXPECT_EQ(reader.read_bits<u8>(3), 0b101u);
    EXPECT_EQ(reader.read_bits<u8>(7), 0b1001001u);
    EXPECT_EQ(reader.read_bits<u16>(14), 0b00111110000001u);
    EXPECT_EQ(reader.bits_remaining(), 0u);
    EXPECT(!reader.has_overrun());
}

TEST_CASE(reads_the_widest_supported_field)
{
    Array<u8, 8> data { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    Media::BitReader reader { data };

    EXPECT_EQ(reader.read_bits<u64>(64), 0x0123456789abcdefull);
    EXPECT(!reader.has_overrun());
}

TEST_CASE(reads_single_bits)
{
    Array<u8, 1> data { 0b1010'0000 };
    Media::BitReader reader { data };

    EXPECT(reader.read_bit());
    EXPECT(!reader.read_bit());
    EXPECT(reader.read_bit());
    EXPECT(!reader.read_bit());
    EXPECT_EQ(reader.bit_position(), 4u);
}

TEST_CASE(skipping_and_aligning_advance_the_position)
{
    Array<u8, 3> data { 0xff, 0b0000'1111, 0xff };
    Media::BitReader reader { data };

    reader.skip_bits(8);
    EXPECT_EQ(reader.bit_position(), 8u);

    reader.read_bits<u8>(4);
    reader.align_to_byte();
    EXPECT_EQ(reader.bit_position(), 16u);

    // Aligning an already-aligned reader does nothing.
    reader.align_to_byte();
    EXPECT_EQ(reader.bit_position(), 16u);
    EXPECT(!reader.has_overrun());
}

TEST_CASE(overrunning_reads_latch_and_consume_the_remainder)
{
    Array<u8, 1> data { 0xff };
    Media::BitReader reader { data };

    EXPECT_EQ(reader.read_bits<u16>(9), 0u);
    EXPECT(reader.has_overrun());
    EXPECT_EQ(reader.bits_remaining(), 0u);

    // Later reads keep yielding zero rather than resuming from the unread bits.
    EXPECT_EQ(reader.read_bits<u8>(1), 0u);
    EXPECT(reader.has_overrun());
}

TEST_CASE(overrunning_skips_latch_as_well)
{
    Array<u8, 2> data { 0xff, 0xff };
    Media::BitReader reader { data };

    reader.skip_bits(17);
    EXPECT(reader.has_overrun());
    EXPECT_EQ(reader.bits_remaining(), 0u);
}

TEST_CASE(an_empty_reader_overruns_immediately)
{
    Media::BitReader reader { ReadonlyBytes {} };

    EXPECT_EQ(reader.bits_remaining(), 0u);
    EXPECT(!reader.has_overrun());
    EXPECT_EQ(reader.read_bits<u8>(1), 0u);
    EXPECT(reader.has_overrun());
}

TEST_CASE(reading_zero_bits_is_a_no_op)
{
    Array<u8, 1> data { 0xff };
    Media::BitReader reader { data };

    EXPECT_EQ(reader.read_bits<u8>(0), 0u);
    EXPECT_EQ(reader.bit_position(), 0u);
    EXPECT(!reader.has_overrun());
}

TEST_CASE(reads_exp_golomb_values)
{
    // 1 | 010 | 011 | 00100 | 00101, encoding 0, 1, 2, 3, 4.
    Array<u8, 3> data { 0b1010'0110, 0b0100'0010, 0b1000'0000 };
    Media::BitReader reader { data };

    EXPECT_EQ(reader.read_exp_golomb(), 0u);
    EXPECT_EQ(reader.read_exp_golomb(), 1u);
    EXPECT_EQ(reader.read_exp_golomb(), 2u);
    EXPECT_EQ(reader.read_exp_golomb(), 3u);
    EXPECT_EQ(reader.read_exp_golomb(), 4u);
    EXPECT(!reader.has_overrun());
}

TEST_CASE(exp_golomb_overruns_on_a_truncated_value)
{
    // Eight leading zeroes with no terminating one bit.
    Array<u8, 1> data { 0 };
    Media::BitReader reader { data };

    EXPECT_EQ(reader.read_exp_golomb(), 0u);
    EXPECT(reader.has_overrun());
}

TEST_CASE(exp_golomb_overruns_rather_than_shifting_out_of_range)
{
    // Thirty-two leading zeroes would make the value exceed what the encoding can represent.
    Array<u8, 8> data { 0, 0, 0, 0, 0x80, 0, 0, 0 };
    Media::BitReader reader { data };

    EXPECT_EQ(reader.read_exp_golomb(), 0u);
    EXPECT(reader.has_overrun());
}

TEST_CASE(reads_leb128_values)
{
    Array<u8, 1> single_byte { 0x7f };
    EXPECT_EQ(Media::BitReader { single_byte }.read_leb128(), 127u);

    // 0xe5 0x8e 0x26 encodes 624485.
    Array<u8, 3> multiple_bytes { 0xe5, 0x8e, 0x26 };
    Media::BitReader reader { multiple_bytes };
    EXPECT_EQ(reader.read_leb128(), 624485u);
    EXPECT(!reader.has_overrun());
}

TEST_CASE(leb128_stops_after_its_final_byte)
{
    Array<u8, 2> data { 0x01, 0xff };
    Media::BitReader reader { data };

    EXPECT_EQ(reader.read_leb128(), 1u);
    EXPECT_EQ(reader.bit_position(), 8u);
}
