/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/CircularBuffer.h>

namespace {

CircularBuffer create_circular_buffer(size_t size)
{
    return MUST(CircularBuffer::create_empty(size));
}

void safe_write(CircularBuffer& buffer, u8 i)
{
    Bytes b { &i, 1 };
    auto written_bytes = buffer.write(b);
    EXPECT_EQ(written_bytes, 1ul);
}

void safe_read(CircularBuffer& buffer, u8 supposed_result)
{
    u8 read_value {};
    Bytes b { &read_value, 1 };
    b = buffer.read(b);
    EXPECT_EQ(b.size(), 1ul);
    EXPECT_EQ(*b.data(), supposed_result);
}

void safe_discard(CircularBuffer& buffer, size_t size)
{
    TRY_OR_FAIL(buffer.discard(size));
}

}

TEST_CASE(simple_write_read)
{
    auto buffer = create_circular_buffer(1);

    safe_write(buffer, 42);
    safe_read(buffer, 42);
}

TEST_CASE(writing_above_limits)
{
    auto buffer = create_circular_buffer(1);

    safe_write(buffer, 1);

    u8 value = 42;
    Bytes b { &value, 1 };
    auto written_bytes = buffer.write(b);
    EXPECT_EQ(written_bytes, 0ul);
}

TEST_CASE(usage_with_wrapping_around)
{
    constexpr size_t capacity = 3;
    auto buffer = create_circular_buffer(capacity);

    for (unsigned i {}; i < capacity; ++i)
        safe_write(buffer, i + 8);

    EXPECT_EQ(buffer.used_space(), capacity);
    EXPECT_EQ(buffer.empty_space(), 0ul);

    safe_read(buffer, 0 + 8);
    safe_read(buffer, 1 + 8);

    EXPECT_EQ(buffer.used_space(), capacity - 2);

    safe_write(buffer, 5);
    safe_write(buffer, 6);

    EXPECT_EQ(buffer.used_space(), capacity);

    safe_read(buffer, 10);
    safe_read(buffer, 5);
    safe_read(buffer, 6);

    EXPECT_EQ(buffer.used_space(), 0ul);
}

TEST_CASE(full_read_aligned)
{
    constexpr size_t capacity = 3;
    auto buffer = create_circular_buffer(capacity);

    for (unsigned i {}; i < capacity; ++i)
        safe_write(buffer, i);

    EXPECT_EQ(buffer.used_space(), capacity);
    EXPECT_EQ(buffer.empty_space(), 0ul);

    u8 const source[] = { 0, 1, 2 };

    u8 result[] = { 0, 0, 0 };
    auto const bytes_or_error = buffer.read({ result, 3 });
    EXPECT_EQ(bytes_or_error.size(), 3ul);

    EXPECT_EQ(memcmp(source, result, 3), 0);
}

TEST_CASE(full_read_non_aligned)
{
    constexpr size_t capacity = 3;
    auto buffer = create_circular_buffer(capacity);

    for (unsigned i {}; i < capacity; ++i)
        safe_write(buffer, i + 5);

    safe_read(buffer, 5);

    safe_write(buffer, 42);

    EXPECT_EQ(buffer.used_space(), capacity);
    EXPECT_EQ(buffer.empty_space(), 0ul);

    u8 result[] = { 0, 0, 0 };
    auto const bytes = buffer.read({ result, 3 });
    EXPECT_EQ(bytes.size(), 3ul);

    u8 const source[] = { 6, 7, 42 };
    EXPECT_EQ(memcmp(source, result, 3), 0);
}

TEST_CASE(full_write_aligned)
{
    constexpr size_t capacity = 3;
    auto buffer = create_circular_buffer(capacity);

    u8 const source[] = { 12, 13, 14 };

    auto written_bytes = buffer.write({ source, 3 });
    EXPECT_EQ(written_bytes, 3ul);

    EXPECT_EQ(buffer.used_space(), capacity);
    EXPECT_EQ(buffer.empty_space(), 0ul);

    for (unsigned i {}; i < capacity; ++i)
        safe_read(buffer, i + 12);

    EXPECT_EQ(buffer.used_space(), 0ul);
}

TEST_CASE(full_write_non_aligned)
{
    constexpr size_t capacity = 3;
    auto buffer = create_circular_buffer(capacity);

    safe_write(buffer, 10);
    safe_read(buffer, 10);

    u8 const source[] = { 12, 13, 14 };

    auto written_bytes = buffer.write({ source, 3 });
    EXPECT_EQ(written_bytes, 3ul);

    EXPECT_EQ(buffer.used_space(), capacity);
    EXPECT_EQ(buffer.empty_space(), 0ul);

    for (unsigned i {}; i < capacity; ++i)
        safe_read(buffer, i + 12);

    EXPECT_EQ(buffer.used_space(), 0ul);
}

TEST_CASE(create_from_bytebuffer)
{
    u8 const source[] = { 2, 4, 6 };
    auto byte_buffer = TRY_OR_FAIL(ByteBuffer::copy(source, AK::array_size(source)));

    auto circular_buffer = TRY_OR_FAIL(CircularBuffer::create_initialized(move(byte_buffer)));
    EXPECT_EQ(circular_buffer.used_space(), circular_buffer.capacity());
    EXPECT_EQ(circular_buffer.used_space(), 3ul);

    safe_read(circular_buffer, 2);
    safe_read(circular_buffer, 4);
    safe_read(circular_buffer, 6);
}

TEST_CASE(discard)
{
    constexpr size_t capacity = 3;
    auto buffer = create_circular_buffer(capacity);

    safe_write(buffer, 11);
    safe_write(buffer, 12);

    safe_discard(buffer, 1);

    safe_read(buffer, 12);

    EXPECT_EQ(buffer.used_space(), 0ul);
    EXPECT_EQ(buffer.empty_space(), capacity);
}

TEST_CASE(discard_on_edge)
{
    constexpr size_t capacity = 3;
    auto buffer = create_circular_buffer(capacity);

    safe_write(buffer, 11);
    safe_write(buffer, 12);
    safe_write(buffer, 13);

    safe_discard(buffer, 2);

    safe_write(buffer, 14);
    safe_write(buffer, 15);

    safe_discard(buffer, 2);

    safe_read(buffer, 15);

    EXPECT_EQ(buffer.used_space(), 0ul);
    EXPECT_EQ(buffer.empty_space(), capacity);
}

TEST_CASE(discard_too_much)
{
    constexpr size_t capacity = 3;
    auto buffer = create_circular_buffer(capacity);

    safe_write(buffer, 11);
    safe_write(buffer, 12);

    safe_discard(buffer, 2);

    auto result = buffer.discard(2);
    EXPECT(result.is_error());
}

TEST_CASE(offset_of)
{
    auto const source = "Well Hello Friends!"sv;
    auto byte_buffer = TRY_OR_FAIL(ByteBuffer::copy(source.bytes()));

    auto circular_buffer = TRY_OR_FAIL(CircularBuffer::create_initialized(byte_buffer));

    auto result = circular_buffer.offset_of("Well"sv);
    EXPECT(result.has_value());
    EXPECT_EQ(result.value(), 0ul);

    result = circular_buffer.offset_of("Hello"sv);
    EXPECT(result.has_value());
    EXPECT_EQ(result.value(), 5ul);

    safe_discard(circular_buffer, 5);

    auto written_bytes = circular_buffer.write(byte_buffer.span().trim(5));
    EXPECT_EQ(written_bytes, 5ul);

    result = circular_buffer.offset_of("!Well"sv);
    EXPECT(result.has_value());
    EXPECT_EQ(result.value(), 13ul);

    result = circular_buffer.offset_of("!Well"sv, {}, 12);
    EXPECT(!result.has_value());

    result = circular_buffer.offset_of("e"sv, 2);
    EXPECT(result.has_value());
    EXPECT_EQ(result.value(), 9ul);
}

TEST_CASE(offset_of_with_until_and_after)
{
    auto const source = "Well Hello Friends!"sv;
    auto byte_buffer = TRY_OR_FAIL(ByteBuffer::copy(source.bytes()));

    auto circular_buffer = TRY_OR_FAIL(CircularBuffer::create_initialized(byte_buffer));

    auto result = circular_buffer.offset_of("Well Hello Friends!"sv, 0, 19);
    EXPECT_EQ(result.value_or(42), 0ul);

    result = circular_buffer.offset_of(" Hello"sv, 4, 10);
    EXPECT_EQ(result.value_or(42), 4ul);

    result = circular_buffer.offset_of("el"sv, 3, 10);
    EXPECT_EQ(result.value_or(42), 6ul);

    safe_discard(circular_buffer, 5);
    auto written_bytes = circular_buffer.write(byte_buffer.span().trim(5));
    EXPECT_EQ(written_bytes, 5ul);

    result = circular_buffer.offset_of("Hello Friends!Well "sv, 0, 19);
    EXPECT_EQ(result.value_or(42), 0ul);

    result = circular_buffer.offset_of("o Frie"sv, 4, 10);
    EXPECT_EQ(result.value_or(42), 4ul);

    result = circular_buffer.offset_of("el"sv, 3, 17);
    EXPECT_EQ(result.value_or(42), 15ul);
}

TEST_CASE(offset_of_with_until_and_after_wrapping_around)
{
    auto const source = "Well Hello Friends!"sv;
    auto byte_buffer = TRY_OR_FAIL(ByteBuffer::copy(source.bytes()));

    auto circular_buffer = TRY_OR_FAIL(CircularBuffer::create_empty(19));

    auto written_bytes = circular_buffer.write(byte_buffer.span().trim(5));
    EXPECT_EQ(written_bytes, 5ul);

    auto result = circular_buffer.offset_of("Well "sv, 0, 5);
    EXPECT_EQ(result.value_or(42), 0ul);

    written_bytes = circular_buffer.write(byte_buffer.span().slice(5));
    EXPECT_EQ(written_bytes, 14ul);

    result = circular_buffer.offset_of("Hello Friends!"sv, 5, 19);
    EXPECT_EQ(result.value_or(42), 5ul);

    safe_discard(circular_buffer, 5);

    result = circular_buffer.offset_of("Hello Friends!"sv, 0, 14);
    EXPECT_EQ(result.value_or(42), 0ul);

    written_bytes = circular_buffer.write(byte_buffer.span().trim(5));
    EXPECT_EQ(written_bytes, 5ul);

    result = circular_buffer.offset_of("Well "sv, 14, 19);
    EXPECT_EQ(result.value_or(42), 14ul);
}

TEST_CASE(find_copy_in_seekback)
{
    auto haystack = "ABABCABCDAB"sv.bytes();
    auto needle = "ABCD"sv.bytes();

    // Set up the buffer for testing.
    auto buffer = MUST(SearchableCircularBuffer::create_empty(haystack.size() + needle.size()));
    auto written_haystack_bytes = buffer.write(haystack);
    VERIFY(written_haystack_bytes == haystack.size());
    MUST(buffer.discard(haystack.size()));
    auto written_needle_bytes = buffer.write(needle);
    VERIFY(written_needle_bytes == needle.size());

    // Note: As of now, the preference during a tie is determined by which algorithm found the match.
    //       Hash-based matching finds the shortest distance first, while memmem finds the greatest distance first.
    //       A matching TODO can be found in CircularBuffer.cpp.

    {
        // Find the largest match with a length between 1 and 1 (all "A").
        auto match = buffer.find_copy_in_seekback(1, 1);
        EXPECT(match.has_value());
        EXPECT_EQ(match.value().distance, 11ul);
        EXPECT_EQ(match.value().length, 1ul);
    }

    {
        // Find the largest match with a length between 1 and 2 (all "AB", everything smaller gets eliminated).
        auto match = buffer.find_copy_in_seekback(2, 1);
        EXPECT(match.has_value());
        EXPECT_EQ(match.value().distance, 11ul);
        EXPECT_EQ(match.value().length, 2ul);
    }

    {
        // Find the largest match with a length between 1 and 3 (all "ABC", everything smaller gets eliminated).
        auto match = buffer.find_copy_in_seekback(3, 1);
        EXPECT(match.has_value());
        EXPECT_EQ(match.value().distance, 6ul);
        EXPECT_EQ(match.value().length, 3ul);
    }

    {
        // Find the largest match with a length between 1 and 4 (all "ABCD", everything smaller gets eliminated).
        auto match = buffer.find_copy_in_seekback(4, 1);
        EXPECT(match.has_value());
        EXPECT_EQ(match.value().distance, 6ul);
        EXPECT_EQ(match.value().length, 4ul);
    }

    {
        // Find the largest match with a length between 1 and 5 (all "ABCD", everything smaller gets eliminated, and nothing larger exists).
        auto match = buffer.find_copy_in_seekback(5, 1);
        EXPECT(match.has_value());
        EXPECT_EQ(match.value().distance, 6ul);
        EXPECT_EQ(match.value().length, 4ul);
    }

    {
        // Find the largest match with a length between 4 and 5 (all "ABCD", everything smaller never gets found, nothing larger exists).
        auto match = buffer.find_copy_in_seekback(5, 4);
        EXPECT(match.has_value());
        EXPECT_EQ(match.value().distance, 6ul);
        EXPECT_EQ(match.value().length, 4ul);
    }

    {
        // Find the largest match with a length between 5 and 5 (nothing is found).
        auto match = buffer.find_copy_in_seekback(5, 5);
        EXPECT(!match.has_value());
    }

    {
        // Find the largest match with a length between 1 and 2 (selected "AB", everything smaller gets eliminated).
        // Since we have a tie, the first qualified match is preferred.
        auto match = buffer.find_copy_in_seekback(Vector<size_t> { 6ul, 9ul }, 2, 1);
        EXPECT_EQ(match.value().distance, 6ul);
        EXPECT_EQ(match.value().length, 2ul);
    }

    {
        // Check that we don't find anything for hints before the valid range.
        auto match = buffer.find_copy_in_seekback(Vector<size_t> { 0ul }, 2, 1);
        EXPECT(!match.has_value());
    }

    {
        // Check that we don't find anything for hints after the valid range.
        auto match = buffer.find_copy_in_seekback(Vector<size_t> { 12ul }, 2, 1);
        EXPECT(!match.has_value());
    }

    {
        // Check that we don't find anything for a minimum length beyond the whole buffer size.
        auto match = buffer.find_copy_in_seekback(12, 13);
        EXPECT(!match.has_value());
    }
}

BENCHMARK_CASE(looping_copy_from_seekback)
{
    auto circular_buffer = MUST(CircularBuffer::create_empty(16 * MiB));

    {
        auto written_bytes = circular_buffer.write("\0"sv.bytes());
        EXPECT_EQ(written_bytes, 1ul);
    }

    {
        auto copied_bytes = TRY_OR_FAIL(circular_buffer.copy_from_seekback(1, 15 * MiB));
        EXPECT_EQ(copied_bytes, 15 * MiB);
    }
}
