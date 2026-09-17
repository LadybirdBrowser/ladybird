/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ImmutableBytes.h>
#include <LibTest/TestCase.h>
#include <string.h>

TEST_CASE(adopt_anonymous_buffer_views_its_bytes)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(64));
    auto const payload = "viewed through ImmutableBytes"sv;
    memcpy(buffer.data<void>(), payload.characters_without_null_termination(), payload.length());

    auto bytes = Core::ImmutableBytes::adopt_anonymous_buffer(buffer);
    EXPECT(bytes.is_valid());
    EXPECT(!bytes.is_file_backed());
    EXPECT(!bytes.is_readonly_mapped());
    EXPECT_EQ(bytes.size(), buffer.size());
    EXPECT_EQ(bytes.bytes().data(), buffer.bytes().data());
    EXPECT_EQ(bytes.bytes().slice(0, payload.length()), payload.bytes());
}

TEST_CASE(adopt_anonymous_buffer_keeps_the_mapping_alive)
{
    auto const payload = "outlives the handle it came from"sv;
    Core::ImmutableBytes bytes;
    {
        auto buffer = MUST(Core::AnonymousBuffer::create_with_size(64));
        memcpy(buffer.data<void>(), payload.characters_without_null_termination(), payload.length());
        bytes = Core::ImmutableBytes::adopt_anonymous_buffer(move(buffer));
    }
    EXPECT_EQ(bytes.bytes().slice(0, payload.length()), payload.bytes());

    auto copy = MUST(bytes.copy_to_byte_buffer());
    EXPECT_EQ(copy.bytes().slice(0, payload.length()), payload.bytes());
}

TEST_CASE(adopt_invalid_anonymous_buffer_is_empty)
{
    auto bytes = Core::ImmutableBytes::adopt_anonymous_buffer(Core::AnonymousBuffer {});
    EXPECT(bytes.is_valid());
    EXPECT(bytes.is_empty());
    EXPECT(bytes.bytes().is_empty());
}
