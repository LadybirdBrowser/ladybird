/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BufferedStream.h>
#include <AK/MemoryStream.h>
#include <AK/String.h>
#include <LibTest/TestCase.h>

TEST_CASE(allocating_memory_stream_empty)
{
    AllocatingMemoryStream stream;

    EXPECT_EQ(stream.used_buffer_size(), 0ul);

    {
        Array<u8, 32> array;
        auto read_bytes = MUST(stream.read_some(array));
        EXPECT_EQ(read_bytes.size(), 0ul);
    }

    {
        auto offset = MUST(stream.offset_of("test"sv.bytes()));
        EXPECT(!offset.has_value());
    }
}

TEST_CASE(allocating_memory_stream_offset_of)
{
    AllocatingMemoryStream stream;
    MUST(stream.write_until_depleted("Well Hello Friends! :^)"sv.bytes()));

    {
        auto offset = MUST(stream.offset_of(" "sv.bytes()));
        EXPECT(offset.has_value());
        EXPECT_EQ(offset.value(), 4ul);
    }

    {
        auto offset = MUST(stream.offset_of("W"sv.bytes()));
        EXPECT(offset.has_value());
        EXPECT_EQ(offset.value(), 0ul);
    }

    {
        auto offset = MUST(stream.offset_of(")"sv.bytes()));
        EXPECT(offset.has_value());
        EXPECT_EQ(offset.value(), 22ul);
    }

    {
        auto offset = MUST(stream.offset_of("-"sv.bytes()));
        EXPECT(!offset.has_value());
    }

    MUST(stream.discard(1));

    {
        auto offset = MUST(stream.offset_of("W"sv.bytes()));
        EXPECT(!offset.has_value());
    }

    {
        auto offset = MUST(stream.offset_of("e"sv.bytes()));
        EXPECT(offset.has_value());
        EXPECT_EQ(offset.value(), 0ul);
    }
}

TEST_CASE(allocating_memory_stream_offset_of_oob)
{
    AllocatingMemoryStream stream;
    // NOTE: This test is to make sure that offset_of() doesn't read past the end of the "initialized" data.
    //       So we have to assume some things about the behavior of this class:
    //       - A chunk is moved to the end when it's fully read from
    //       - A free chunk is used as-is, no new ones are allocated if one exists.

    // First, fill exactly one chunk (in groups of 16 bytes).
    for (size_t i = 0; i < AllocatingMemoryStream::CHUNK_SIZE / 16; ++i)
        MUST(stream.write_until_depleted("AAAAAAAAAAAAAAAA"sv.bytes()));

    // Then discard it all.
    MUST(stream.discard(AllocatingMemoryStream::CHUNK_SIZE));
    // Now we can write into this chunk again, knowing that it's initialized to all 'A's.
    MUST(stream.write_until_depleted("Well Hello Friends! :^)"sv.bytes()));

    {
        auto offset = MUST(stream.offset_of("A"sv.bytes()));
        EXPECT(!offset.has_value());
    }
}

TEST_CASE(allocating_memory_stream_offset_of_after_chunk_reorder)
{
    AllocatingMemoryStream stream;

    // First, fill exactly one chunk (in groups of 16 bytes). This chunk will be reordered.
    for (size_t i = 0; i < AllocatingMemoryStream::CHUNK_SIZE / 16; ++i)
        MUST(stream.write_until_depleted("AAAAAAAAAAAAAAAA"sv.bytes()));

    // Append a few additional bytes to create a second chunk.
    MUST(stream.write_until_depleted("BCDEFGHIJKLMNOPQ"sv.bytes()));

    // Read back the first chunk, which should reorder it to the end of the list.
    // The chunk that we wrote to the second time is now the first one.
    MUST(stream.discard(AllocatingMemoryStream::CHUNK_SIZE));

    {
        auto offset = MUST(stream.offset_of("A"sv.bytes()));
        EXPECT(!offset.has_value());
    }

    {
        auto offset = MUST(stream.offset_of("B"sv.bytes()));
        EXPECT(offset.has_value());
        EXPECT_EQ(offset.value(), 0ul);
    }

    {
        auto offset = MUST(stream.offset_of("Q"sv.bytes()));
        EXPECT(offset.has_value());
        EXPECT_EQ(offset.value(), 15ul);
    }
}

TEST_CASE(allocating_memory_stream_offset_of_with_write_offset_multiple_of_chunk_size)
{
    // This tests a specific edge case where we would erroneously trim the last searched block
    // to size 0 if the current write offset is a multiple of the chunk size.

    AllocatingMemoryStream stream;

    // First, fill exactly one chunk (in groups of 16 bytes).
    for (size_t i = 0; i < (AllocatingMemoryStream::CHUNK_SIZE / 16) - 1; ++i)
        MUST(stream.write_until_depleted("AAAAAAAAAAAAAAAA"sv.bytes()));
    MUST(stream.write_until_depleted("BCDEFGHIJKLMNOPQ"sv.bytes()));

    // Read a few bytes from the beginning to ensure that we are trying to slice into the zero-sized block.
    MUST(stream.discard(32));

    {
        auto offset = MUST(stream.offset_of("B"sv.bytes()));
        EXPECT(offset.has_value());
        EXPECT_EQ(offset.value(), AllocatingMemoryStream::CHUNK_SIZE - 32 - 16);
    }

    {
        auto offset = MUST(stream.offset_of("Q"sv.bytes()));
        EXPECT(offset.has_value());
        EXPECT_EQ(offset.value(), AllocatingMemoryStream::CHUNK_SIZE - 32 - 1);
    }
}

TEST_CASE(fixed_memory_read_write)
{
    constexpr auto some_words = "These are some words"sv;

    auto empty = TRY_OR_FAIL(ByteBuffer::create_uninitialized(some_words.length()));
    FixedMemoryStream stream { empty.bytes() };

    ReadonlyBytes buffer { some_words.characters_without_null_termination(), some_words.length() };
    TRY_OR_FAIL(stream.write_some(buffer));

    EXPECT_EQ(TRY_OR_FAIL(stream.tell()), some_words.length());
    EXPECT(stream.is_eof());

    TRY_OR_FAIL(stream.seek(0));
    auto contents = TRY_OR_FAIL(stream.read_until_eof());
    EXPECT_EQ(contents.bytes(), some_words.bytes());
}

TEST_CASE(fixed_memory_close)
{
    auto empty = TRY_OR_FAIL(ByteBuffer::create_uninitialized(64));
    FixedMemoryStream stream { empty.bytes() };
    EXPECT(stream.is_open());
    stream.close();
    EXPECT(stream.is_open());
}

TEST_CASE(fixed_memory_read_only)
{
    constexpr auto some_words = "These are some words"sv;

    FixedMemoryStream stream { ReadonlyBytes { some_words.bytes() } };

    auto contents = TRY_OR_FAIL(stream.read_until_eof());
    EXPECT_EQ(contents.bytes(), some_words.bytes());

    TRY_OR_FAIL(stream.seek(0));
    ReadonlyBytes buffer { some_words.characters_without_null_termination(), some_words.length() };
    EXPECT(stream.write_some(buffer).is_error());
    EXPECT_EQ(TRY_OR_FAIL(stream.tell()), 0ull);
    EXPECT(!stream.is_eof());
}

TEST_CASE(fixed_memory_seeking_around)
{
    auto stream_buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(8702ul));
    FixedMemoryStream stream { ReadonlyBytes { stream_buffer.bytes() } };

    auto buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(16));

    TRY_OR_FAIL(stream.seek(500, SeekMode::SetPosition));
    EXPECT_EQ(stream.tell().release_value(), 500ul);
    TRY_OR_FAIL(stream.read_until_filled(buffer));

    TRY_OR_FAIL(stream.seek(234, SeekMode::FromCurrentPosition));
    EXPECT_EQ(stream.tell().release_value(), 750ul);
    TRY_OR_FAIL(stream.read_until_filled(buffer));

    TRY_OR_FAIL(stream.seek(-105, SeekMode::FromEndPosition));
    EXPECT_EQ(stream.tell().release_value(), 8597ul);
    TRY_OR_FAIL(stream.read_until_filled(buffer));
}

BENCHMARK_CASE(fixed_memory_tell)
{
    auto stream_buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(10 * KiB));
    FixedMemoryStream stream { ReadonlyBytes { stream_buffer.bytes() } };

    auto expected_fixed_memory_offset = 0u;
    auto ten_byte_buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(1));
    for (auto i = 0u; i < 4000; ++i) {
        TRY_OR_FAIL(stream.read_until_filled(ten_byte_buffer));
        expected_fixed_memory_offset += 1u;
        EXPECT_EQ(expected_fixed_memory_offset, TRY_OR_FAIL(stream.tell()));
    }

    for (auto i = 0u; i < 4000; ++i) {
        auto seek_fixed_memory_offset = TRY_OR_FAIL(stream.seek(-1, SeekMode::FromCurrentPosition));
        expected_fixed_memory_offset -= 1;
        EXPECT_EQ(seek_fixed_memory_offset, TRY_OR_FAIL(stream.tell()));
        EXPECT_EQ(expected_fixed_memory_offset, TRY_OR_FAIL(stream.tell()));
    }
}

TEST_CASE(fixed_memory_truncate)
{
    auto stream_buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(10 * KiB));
    FixedMemoryStream stream { ReadonlyBytes { stream_buffer.bytes() } };

    EXPECT(stream.truncate(999).is_error());
}

TEST_CASE(fixed_memory_read_in_place)
{
    constexpr auto some_words = "These are some words"sv;

    FixedMemoryStream readonly_stream { ReadonlyBytes { some_words.bytes() } };

    // Trying to read mutable values from a read-only stream should fail.
    EXPECT(readonly_stream.read_in_place<u8>(1).is_error());
    EXPECT_EQ(readonly_stream.offset(), 0u);

    // Reading const values should succeed.
    auto characters = TRY_OR_FAIL(readonly_stream.read_in_place<u8 const>(20));
    EXPECT_EQ(characters, some_words.bytes());
    EXPECT(readonly_stream.is_eof());

    FixedMemoryStream mutable_stream { Bytes { const_cast<u8*>(some_words.bytes().data()), some_words.bytes().size() }, FixedMemoryStream::Mode::ReadWrite };
    // Trying to read mutable values from a mutable stream should succeed.
    TRY_OR_FAIL(mutable_stream.read_in_place<u8>(1));
    EXPECT_EQ(mutable_stream.offset(), 1u);
    TRY_OR_FAIL(mutable_stream.seek(0));

    // Reading const values should succeed.
    auto characters_again = TRY_OR_FAIL(mutable_stream.read_in_place<u8 const>(20));
    EXPECT_EQ(characters_again, some_words.bytes());
    EXPECT(mutable_stream.is_eof());
}

TEST_CASE(buffered_memory_stream_read_line)
{
    auto array = Array<u8, 32> {};

    // First line: 8 bytes, second line: 24 bytes
    array.fill('A');
    array[7] = '\n';
    array[31] = '\n';

    auto memory_stream = make<FixedMemoryStream>(array.span(), FixedMemoryStream::Mode::ReadOnly);

    // Buffer for buffered seekable is larger than the stream, so stream goes EOF immediately on read
    auto buffered_stream = TRY_OR_FAIL(InputBufferedSeekable<FixedMemoryStream>::create(move(memory_stream), 64));

    // Buffer is only 16 bytes, first read succeeds, second fails
    auto buffer = TRY_OR_FAIL(ByteBuffer::create_zeroed(16));

    auto read_bytes = TRY_OR_FAIL(buffered_stream->read_line(buffer));

    EXPECT_EQ(read_bytes, "AAAAAAA"sv);

    auto read_or_error = buffered_stream->read_line(buffer);

    EXPECT(read_or_error.is_error());
    EXPECT_EQ(read_or_error.error().code(), EMSGSIZE);
}

TEST_CASE(buffered_memory_stream_read_line_with_resizing_where_stream_buffer_is_sufficient)
{
    auto array = Array<u8, 24> {};

    // The first line is 8 A's, the second line is 14 A's, two bytes are newline characters.
    array.fill('A');
    array[8] = '\n';
    array[23] = '\n';

    auto memory_stream = make<FixedMemoryStream>(array.span(), FixedMemoryStream::Mode::ReadOnly);

    auto buffered_stream = TRY_OR_FAIL(InputBufferedSeekable<FixedMemoryStream>::create(move(memory_stream), 64));

    size_t initial_buffer_size = 4;
    auto buffer = TRY_OR_FAIL(ByteBuffer::create_zeroed(initial_buffer_size));

    auto read_bytes = TRY_OR_FAIL(buffered_stream->read_line_with_resize(buffer));

    // The first line, which is 8 A's, should be read in.
    EXPECT_EQ(read_bytes, "AAAAAAAA"sv);

    read_bytes = TRY_OR_FAIL(buffered_stream->read_line_with_resize(buffer));

    // The second line, which is 14 A's, should be read in.
    EXPECT_EQ(read_bytes, "AAAAAAAAAAAAAA"sv);

    // A resize should have happened because the user supplied buffer was too small.
    EXPECT(buffer.size() > initial_buffer_size);

    // Reading from the stream again should return an empty StringView.
    read_bytes = TRY_OR_FAIL(buffered_stream->read_line_with_resize(buffer));
    EXPECT(read_bytes.is_empty());
}

TEST_CASE(buffered_memory_stream_read_line_with_resizing_where_stream_buffer_is_not_sufficient)
{
    // This is the same as "buffered_memory_stream_read_line_with_resizing_where_stream_buffer_is_sufficient"
    // but with a smaller stream buffer, meaning that the line must be read into the user supplied
    // buffer in chunks. All assertions and invariants should remain unchanged.
    auto array = Array<u8, 24> {};

    // The first line is 8 A's, the second line is 14 A's, two bytes are newline characters.
    array.fill('A');
    array[8] = '\n';
    array[23] = '\n';

    auto memory_stream = make<FixedMemoryStream>(array.span(), FixedMemoryStream::Mode::ReadOnly);

    auto buffered_stream = TRY_OR_FAIL(InputBufferedSeekable<FixedMemoryStream>::create(move(memory_stream), 6));

    size_t initial_buffer_size = 4;
    auto buffer = TRY_OR_FAIL(ByteBuffer::create_zeroed(initial_buffer_size));

    auto read_bytes = TRY_OR_FAIL(buffered_stream->read_line_with_resize(buffer));

    // The first line, which is 8 A's, should be read in.
    EXPECT_EQ(read_bytes, "AAAAAAAA"sv);

    read_bytes = TRY_OR_FAIL(buffered_stream->read_line_with_resize(buffer));

    // The second line, which is 14 A's, should be read in.
    EXPECT_EQ(read_bytes, "AAAAAAAAAAAAAA"sv);

    // A resize should have happened because the user supplied buffer was too small.
    EXPECT(buffer.size() > initial_buffer_size);

    // Reading from the stream again should return an empty StringView.
    read_bytes = TRY_OR_FAIL(buffered_stream->read_line_with_resize(buffer));
    EXPECT(read_bytes.is_empty());
}

TEST_CASE(buffered_memory_stream_read_line_with_resizing_with_no_newline_where_stream_buffer_is_sufficient)
{
    auto array = Array<u8, 24> {};

    array.fill('A');

    auto memory_stream = make<FixedMemoryStream>(array.span(), FixedMemoryStream::Mode::ReadOnly);

    auto buffered_stream = TRY_OR_FAIL(InputBufferedSeekable<FixedMemoryStream>::create(move(memory_stream), 64));

    size_t initial_buffer_size = 4;
    auto buffer = TRY_OR_FAIL(ByteBuffer::create_zeroed(initial_buffer_size));

    auto read_bytes = TRY_OR_FAIL(buffered_stream->read_line_with_resize(buffer));

    // All the contents of the buffer should have been read in.
    EXPECT_EQ(read_bytes.length(), array.size());

    // Reading from the stream again should return an empty StringView.
    read_bytes = TRY_OR_FAIL(buffered_stream->read_line_with_resize(buffer));
    EXPECT(read_bytes.is_empty());
}

TEST_CASE(buffered_memory_stream_read_line_with_resizing_with_no_newline_where_stream_buffer_is_not_sufficient)
{
    // This should behave as buffered_memory_stream_read_line_with_resizing_with_no_newline_where_stream_buffer_is_sufficient
    // but the internal buffer of the stream must be copied over in chunks.
    auto array = Array<u8, 24> {};

    array.fill('A');

    auto memory_stream = make<FixedMemoryStream>(array.span(), FixedMemoryStream::Mode::ReadOnly);

    auto buffered_stream = TRY_OR_FAIL(InputBufferedSeekable<FixedMemoryStream>::create(move(memory_stream), 6));

    size_t initial_buffer_size = 4;
    auto buffer = TRY_OR_FAIL(ByteBuffer::create_zeroed(initial_buffer_size));

    auto read_bytes = TRY_OR_FAIL(buffered_stream->read_line_with_resize(buffer));

    // All the contents of the buffer should have been read in.
    EXPECT_EQ(read_bytes.length(), array.size());

    // Reading from the stream again should return an empty StringView.
    read_bytes = TRY_OR_FAIL(buffered_stream->read_line_with_resize(buffer));
    EXPECT(read_bytes.is_empty());
}
