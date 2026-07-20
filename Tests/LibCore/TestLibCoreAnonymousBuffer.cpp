/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <string.h>

TEST_CASE(create_with_size)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(1024));
    EXPECT(buffer.is_valid());
    EXPECT_NE(buffer.fd(), -1);
    EXPECT_EQ(buffer.size(), 1024u);
    EXPECT_NE(buffer.data<void>(), nullptr);
    EXPECT_EQ(buffer.bytes().size(), 1024u);
}

TEST_CASE(create_with_size_produces_independent_buffers)
{
    Vector<Core::AnonymousBuffer> buffers;
    for (u8 i = 0; i < 64; ++i) {
        auto buffer = MUST(Core::AnonymousBuffer::create_with_size(64));
        EXPECT(buffer.is_valid());
        *buffer.data<u8>() = i;
        buffers.append(move(buffer));
    }

    for (u8 i = 0; i < 64; ++i)
        EXPECT_EQ(*buffers[i].data<u8>(), i);
}

TEST_CASE(default_constructed_is_invalid)
{
    Core::AnonymousBuffer buffer;
    EXPECT(!buffer.is_valid());
    EXPECT_EQ(buffer.fd(), -1);
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.data<void>(), nullptr);
    EXPECT(buffer.bytes().is_empty());
}

TEST_CASE(read_and_write_contents)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(64));

    auto const payload = "hello, anonymous buffer!"sv;
    memcpy(buffer.data<void>(), payload.characters_without_null_termination(), payload.length());

    auto const* data = buffer.data<char const>();
    StringView read_back { data, payload.length() };
    EXPECT_EQ(read_back, payload);
}

TEST_CASE(create_with_zero_size)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(0));
    EXPECT(buffer.is_valid());
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT(buffer.bytes().is_empty());

    auto fd = MUST(Core::System::dup(buffer.fd()));
    auto mirror = MUST(Core::AnonymousBuffer::create_from_anon_fd(fd, 0));
    EXPECT(mirror.is_valid());
    EXPECT_EQ(mirror.size(), 0u);
    EXPECT(mirror.bytes().is_empty());
}

TEST_CASE(reconstruct_from_anon_fd_shares_memory)
{
    auto original = MUST(Core::AnonymousBuffer::create_with_size(128));

    auto const payload = "shared across mappings"sv;
    memcpy(original.data<void>(), payload.characters_without_null_termination(), payload.length());

    auto fd = MUST(Core::System::dup(original.fd()));
    auto mirror = MUST(Core::AnonymousBuffer::create_from_anon_fd(fd, original.size()));

    EXPECT(mirror.is_valid());
    EXPECT_EQ(mirror.size(), original.size());

    StringView mirrored { mirror.data<char const>(), payload.length() };
    EXPECT_EQ(mirrored, payload);
}
