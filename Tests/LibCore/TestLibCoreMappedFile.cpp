/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/MaybeOwned.h>
#include <AK/String.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibTest/TestCase.h>
#include <fcntl.h>
#include <unistd.h>

TEST_CASE(mapped_file_open)
{
    // Fill the file with a little content so we can write to it.
    constexpr auto text = "Here's some text to be mmapped."sv;
    {
        auto maybe_file = Core::File::open("/tmp/file-open-test.txt"sv, Core::File::OpenMode::Write);
        if (maybe_file.is_error()) {
            warnln("Failed to open the file: {}", strerror(maybe_file.error().code()));
            VERIFY_NOT_REACHED();
        }
        TRY_OR_FAIL(maybe_file.value()->write_until_depleted(text.bytes()));
    }

    auto maybe_file = Core::MappedFile::map("/tmp/file-open-test.txt"sv, Core::MappedFile::Mode::ReadWrite);
    if (maybe_file.is_error()) {
        warnln("Failed to open the file: {}", strerror(maybe_file.error().code()));
        VERIFY_NOT_REACHED();
    }

    // Testing out some basic file properties.
    auto file = maybe_file.release_value();
    EXPECT(file->is_open());
    EXPECT(!file->is_eof());

    auto size = TRY_OR_FAIL(file->size());
    EXPECT_EQ(size, text.length());
}

constexpr auto expected_buffer_contents = "&lt;small&gt;(Please consider translating this message for the benefit of your fellow Wikimedians. Please also consider translating"sv;

TEST_CASE(mapped_file_read_bytes)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map("./long_lines.txt"sv, Core::MappedFile::Mode::ReadOnly));

    auto buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(131));

    auto result = file->read_some(buffer);
    EXPECT_EQ(TRY_OR_FAIL(result).size(), 131ul);

    StringView buffer_contents { buffer.bytes() };
    EXPECT_EQ(buffer_contents, expected_buffer_contents);
}

constexpr auto expected_seek_contents1 = "|Lleer esti mens"sv;
constexpr auto expected_seek_contents2 = "s of advanced ad"sv;
constexpr auto expected_seek_contents3 = "levels of advanc"sv;

TEST_CASE(mapped_file_seeking_around)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map("./long_lines.txt"sv, Core::MappedFile::Mode::ReadOnly));

    EXPECT_EQ(file->size().release_value(), 8702ul);

    auto buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(16));

    StringView buffer_contents { buffer.bytes() };

    TRY_OR_FAIL(file->seek(500, SeekMode::SetPosition));
    EXPECT_EQ(file->tell().release_value(), 500ul);
    TRY_OR_FAIL(file->read_until_filled(buffer));
    EXPECT_EQ(buffer_contents, expected_seek_contents1);

    TRY_OR_FAIL(file->seek(234, SeekMode::FromCurrentPosition));
    EXPECT_EQ(file->tell().release_value(), 750ul);
    TRY_OR_FAIL(file->read_until_filled(buffer));
    EXPECT_EQ(buffer_contents, expected_seek_contents2);

    TRY_OR_FAIL(file->seek(-105, SeekMode::FromEndPosition));
    EXPECT_EQ(file->tell().release_value(), 8597ul);
    TRY_OR_FAIL(file->read_until_filled(buffer));
    EXPECT_EQ(buffer_contents, expected_seek_contents3);
}

BENCHMARK_CASE(file_tell)
{
    auto file = TRY_OR_FAIL(Core::MappedFile::map("./10kb.txt"sv, Core::MappedFile::Mode::ReadOnly));
    auto expected_file_offset = 0u;
    auto ten_byte_buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(1));
    for (auto i = 0u; i < 4000; ++i) {
        TRY_OR_FAIL(file->read_until_filled(ten_byte_buffer));
        expected_file_offset += 1u;
        EXPECT_EQ(expected_file_offset, TRY_OR_FAIL(file->tell()));
    }

    for (auto i = 0u; i < 4000; ++i) {
        auto seek_file_offset = TRY_OR_FAIL(file->seek(-1, SeekMode::FromCurrentPosition));
        expected_file_offset -= 1;
        EXPECT_EQ(seek_file_offset, TRY_OR_FAIL(file->tell()));
        EXPECT_EQ(expected_file_offset, TRY_OR_FAIL(file->tell()));
    }
}

TEST_CASE(mapped_file_adopt_fd)
{
    int rc = ::open("./long_lines.txt", O_RDONLY);
    EXPECT(rc >= 0);

    auto file = TRY_OR_FAIL(Core::MappedFile::map_from_fd_and_close(rc, "./long_lines.txt"sv));

    EXPECT_EQ(file->size().release_value(), 8702ul);

    auto buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(16));

    StringView buffer_contents { buffer.bytes() };

    TRY_OR_FAIL(file->seek(500, SeekMode::SetPosition));
    EXPECT_EQ(file->tell().release_value(), 500ul);
    TRY_OR_FAIL(file->read_until_filled(buffer));
    EXPECT_EQ(buffer_contents, expected_seek_contents1);

    // A single seek & read test should be fine for now.
}

TEST_CASE(mapped_file_adopt_invalid_fd)
{
    auto maybe_file = Core::MappedFile::map_from_fd_and_close(-1, "./long_lines.txt"sv);
    EXPECT(maybe_file.is_error());
    EXPECT_EQ(maybe_file.error().code(), EBADF);
}

TEST_CASE(mapped_file_tell_and_seek)
{
    auto mapped_file = Core::MappedFile::map("./small.txt"sv).release_value();

    // Initial state.
    {
        auto current_offset = mapped_file->tell().release_value();
        EXPECT_EQ(current_offset, 0ul);
    }

    // Read a character.
    {
        auto character = mapped_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'W');
        auto current_offset = mapped_file->tell().release_value();
        EXPECT_EQ(current_offset, 1ul);
    }

    // Read one more character.
    {
        auto character = mapped_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'e');
        auto current_offset = mapped_file->tell().release_value();
        EXPECT_EQ(current_offset, 2ul);
    }

    // Seek seven characters forward.
    {
        auto current_offset = mapped_file->seek(7, SeekMode::FromCurrentPosition).release_value();
        EXPECT_EQ(current_offset, 9ul);
    }

    // Read a character again.
    {
        auto character = mapped_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'o');
        auto current_offset = mapped_file->tell().release_value();
        EXPECT_EQ(current_offset, 10ul);
    }

    // Seek five characters backwards.
    {
        auto current_offset = mapped_file->seek(-5, SeekMode::FromCurrentPosition).release_value();
        EXPECT_EQ(current_offset, 5ul);
    }

    // Read a character.
    {
        auto character = mapped_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'h');
        auto current_offset = mapped_file->tell().release_value();
        EXPECT_EQ(current_offset, 6ul);
    }

    // Seek back to the beginning.
    {
        auto current_offset = mapped_file->seek(0, SeekMode::SetPosition).release_value();
        EXPECT_EQ(current_offset, 0ul);
    }

    // Read the first character. This should prime the buffer if it hasn't happened already.
    {
        auto character = mapped_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'W');
        auto current_offset = mapped_file->tell().release_value();
        EXPECT_EQ(current_offset, 1ul);
    }

    // Seek beyond the buffer size, which should invalidate the buffer.
    {
        auto current_offset = mapped_file->seek(12, SeekMode::SetPosition).release_value();
        EXPECT_EQ(current_offset, 12ul);
    }

    // Ensure that we still read the correct contents from the new offset with a (presumably) freshly filled buffer.
    {
        auto character = mapped_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'r');
        auto current_offset = mapped_file->tell().release_value();
        EXPECT_EQ(current_offset, 13ul);
    }
}
