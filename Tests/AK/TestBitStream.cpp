/*
 * Copyright (c) 2023, Tim Schumacher <timschumi@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitStream.h>
#include <AK/MemoryStream.h>
#include <LibTest/TestCase.h>

using namespace Test::Randomized;

// Note: This does not do any checks on the internal representation, it just ensures that the behavior of the input and output streams match.
TEST_CASE(little_endian_bit_stream_input_output_match)
{
    auto memory_stream = make<AllocatingMemoryStream>();

    // Note: The bit stream only ever reads from/writes to the underlying stream in one byte chunks,
    // so testing with sizes that will not trigger a write will yield unexpected results.
    LittleEndianOutputBitStream bit_write_stream { MaybeOwned<Stream>(*memory_stream) };
    LittleEndianInputBitStream bit_read_stream { MaybeOwned<Stream>(*memory_stream) };

    // Test two mirrored chunks of a fully mirrored pattern to check that we are not dropping bits.
    {
        MUST(bit_write_stream.write_bits(0b1111u, 4));
        MUST(bit_write_stream.write_bits(0b1111u, 4));
        MUST(bit_write_stream.flush_buffer_to_stream());

        auto result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b1111u, result);
        result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b1111u, result);
    }
    {
        MUST(bit_write_stream.write_bits(0b0000u, 4));
        MUST(bit_write_stream.write_bits(0b0000u, 4));
        MUST(bit_write_stream.flush_buffer_to_stream());

        auto result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b0000u, result);
        result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b0000u, result);
    }

    // Test two mirrored chunks of a non-mirrored pattern to check that we are writing bits within a pattern in the correct order.
    {
        MUST(bit_write_stream.write_bits(0b1000u, 4));
        MUST(bit_write_stream.write_bits(0b1000u, 4));
        MUST(bit_write_stream.flush_buffer_to_stream());

        auto result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b1000u, result);
        result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b1000u, result);
    }

    // Test two different chunks to check that we are not confusing their order.
    {
        MUST(bit_write_stream.write_bits(0b1000u, 4));
        MUST(bit_write_stream.write_bits(0b0100u, 4));
        MUST(bit_write_stream.flush_buffer_to_stream());

        auto result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b1000u, result);
        result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b0100u, result);
    }

    // Test a pattern that spans multiple bytes.
    {
        MUST(bit_write_stream.write_bits(0b1101001000100001u, 16));
        MUST(bit_write_stream.flush_buffer_to_stream());

        auto result = MUST(bit_read_stream.read_bits(16));
        EXPECT_EQ(0b1101001000100001u, result);
    }
}

// Note: This does not do any checks on the internal representation, it just ensures that the behavior of the input and output streams match.
TEST_CASE(big_endian_bit_stream_input_output_match)
{
    auto memory_stream = make<AllocatingMemoryStream>();

    // Note: The bit stream only ever reads from/writes to the underlying stream in one byte chunks,
    // so testing with sizes that will not trigger a write will yield unexpected results.
    BigEndianOutputBitStream bit_write_stream { MaybeOwned<Stream>(*memory_stream) };
    BigEndianInputBitStream bit_read_stream { MaybeOwned<Stream>(*memory_stream) };

    // Test two mirrored chunks of a fully mirrored pattern to check that we are not dropping bits.
    {
        MUST(bit_write_stream.write_bits(0b1111u, 4));
        MUST(bit_write_stream.write_bits(0b1111u, 4));
        auto result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b1111u, result);
        result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b1111u, result);
    }
    {
        MUST(bit_write_stream.write_bits(0b0000u, 4));
        MUST(bit_write_stream.write_bits(0b0000u, 4));
        auto result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b0000u, result);
        result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b0000u, result);
    }

    // Test two mirrored chunks of a non-mirrored pattern to check that we are writing bits within a pattern in the correct order.
    {
        MUST(bit_write_stream.write_bits(0b1000u, 4));
        MUST(bit_write_stream.write_bits(0b1000u, 4));
        auto result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b1000u, result);
        result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b1000u, result);
    }

    // Test two different chunks to check that we are not confusing their order.
    {
        MUST(bit_write_stream.write_bits(0b1000u, 4));
        MUST(bit_write_stream.write_bits(0b0100u, 4));
        auto result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b1000u, result);
        result = MUST(bit_read_stream.read_bits(4));
        EXPECT_EQ(0b0100u, result);
    }

    // Test a pattern that spans multiple bytes.
    {
        MUST(bit_write_stream.write_bits(0b1101001000100001u, 16));
        auto result = MUST(bit_read_stream.read_bits(16));
        EXPECT_EQ(0b1101001000100001u, result);
    }
}

TEST_CASE(bit_reads_beyond_stream_limits)
{
    Array<u8, 1> const test_data { 0xFF };

    {
        auto memory_stream = make<FixedMemoryStream>(test_data);
        auto bit_stream = make<LittleEndianInputBitStream>(move(memory_stream), LittleEndianInputBitStream::UnsatisfiableReadBehavior::Reject);

        {
            auto result = TRY_OR_FAIL(bit_stream->read_bits<u8>(6));
            EXPECT_EQ(result, 0b111111);
        }

        {
            auto result = bit_stream->read_bits<u8>(6);
            EXPECT(result.is_error());
        }

        {
            auto result = bit_stream->read_bits<u8>(6);
            EXPECT(result.is_error());
        }
    }

    {
        // LittleEndianInputBitStream allows reading null bits beyond the original data
        // for compatibility purposes if enabled.
        auto memory_stream = make<FixedMemoryStream>(test_data);
        auto bit_stream = make<LittleEndianInputBitStream>(move(memory_stream), LittleEndianInputBitStream::UnsatisfiableReadBehavior::FillWithZero);

        {
            auto result = TRY_OR_FAIL(bit_stream->read_bits<u8>(6));
            EXPECT_EQ(result, 0b111111);
        }

        {
            auto result = TRY_OR_FAIL(bit_stream->read_bits<u8>(6));
            EXPECT_EQ(result, 0b000011);
        }

        {
            auto result = TRY_OR_FAIL(bit_stream->read_bits<u8>(6));
            EXPECT_EQ(result, 0b000000);
        }
    }

    {
        auto memory_stream = make<FixedMemoryStream>(test_data);
        auto bit_stream = make<BigEndianInputBitStream>(move(memory_stream));

        {
            auto result = TRY_OR_FAIL(bit_stream->read_bits<u8>(6));
            EXPECT_EQ(result, 0b111111);
        }

        {
            auto result = bit_stream->read_bits<u8>(6);
            EXPECT(result.is_error());
        }

        {
            auto result = bit_stream->read_bits<u8>(6);
            EXPECT(result.is_error());
        }
    }
}

RANDOMIZED_TEST_CASE(roundtrip_u8_little_endian)
{
    GEN(n, Gen::number_u64(NumericLimits<u8>::max()));

    auto memory_stream = make<AllocatingMemoryStream>();
    LittleEndianOutputBitStream sut_write { MaybeOwned<Stream>(*memory_stream) };
    LittleEndianInputBitStream sut_read { MaybeOwned<Stream>(*memory_stream) };

    MUST(sut_write.write_bits(n, 8));
    MUST(sut_write.flush_buffer_to_stream());
    auto result = MUST(sut_read.read_bits<u64>(8));

    EXPECT_EQ(result, n);
}

RANDOMIZED_TEST_CASE(roundtrip_u16_little_endian)
{
    GEN(n, Gen::number_u64(NumericLimits<u16>::max()));

    auto memory_stream = make<AllocatingMemoryStream>();
    LittleEndianOutputBitStream sut_write { MaybeOwned<Stream>(*memory_stream) };
    LittleEndianInputBitStream sut_read { MaybeOwned<Stream>(*memory_stream) };

    MUST(sut_write.write_bits(n, 16));
    MUST(sut_write.flush_buffer_to_stream());
    auto result = MUST(sut_read.read_bits<u64>(16));

    EXPECT_EQ(result, n);
}

RANDOMIZED_TEST_CASE(roundtrip_u32_little_endian)
{
    GEN(n, Gen::number_u64(NumericLimits<u32>::max()));

    auto memory_stream = make<AllocatingMemoryStream>();
    LittleEndianOutputBitStream sut_write { MaybeOwned<Stream>(*memory_stream) };
    LittleEndianInputBitStream sut_read { MaybeOwned<Stream>(*memory_stream) };

    MUST(sut_write.write_bits(n, 32));
    MUST(sut_write.flush_buffer_to_stream());
    auto result = MUST(sut_read.read_bits<u64>(32));

    EXPECT_EQ(result, n);
}

RANDOMIZED_TEST_CASE(roundtrip_u8_big_endian)
{
    GEN(n, Gen::number_u64(NumericLimits<u8>::max()));

    auto memory_stream = make<AllocatingMemoryStream>();
    BigEndianOutputBitStream sut_write { MaybeOwned<Stream>(*memory_stream) };
    BigEndianInputBitStream sut_read { MaybeOwned<Stream>(*memory_stream) };

    MUST(sut_write.write_bits(n, 8));
    auto result = MUST(sut_read.read_bits<u64>(8));

    EXPECT_EQ(result, n);
}

RANDOMIZED_TEST_CASE(roundtrip_u16_big_endian)
{
    GEN(n, Gen::number_u64(NumericLimits<u16>::max()));

    auto memory_stream = make<AllocatingMemoryStream>();
    BigEndianOutputBitStream sut_write { MaybeOwned<Stream>(*memory_stream) };
    BigEndianInputBitStream sut_read { MaybeOwned<Stream>(*memory_stream) };

    MUST(sut_write.write_bits(n, 16));
    auto result = MUST(sut_read.read_bits<u64>(16));

    EXPECT_EQ(result, n);
}

RANDOMIZED_TEST_CASE(roundtrip_u32_big_endian)
{
    GEN(n, Gen::number_u64(NumericLimits<u32>::max()));

    auto memory_stream = make<AllocatingMemoryStream>();
    BigEndianOutputBitStream sut_write { MaybeOwned<Stream>(*memory_stream) };
    BigEndianInputBitStream sut_read { MaybeOwned<Stream>(*memory_stream) };

    MUST(sut_write.write_bits(n, 32));
    auto result = MUST(sut_read.read_bits<u64>(32));

    EXPECT_EQ(result, n);
}
