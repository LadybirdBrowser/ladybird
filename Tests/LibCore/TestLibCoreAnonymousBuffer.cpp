/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/MappedFile.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <string.h>

#ifdef AK_OS_WINDOWS
#    include <AK/Windows.h>
#else
#    include <fcntl.h>
#endif

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

TEST_CASE(map_from_anon_fd_as_read_only)
{
    auto original = MUST(Core::AnonymousBuffer::create_with_size(128));

    auto const payload = "mapped read-only"sv;
    memcpy(original.data<void>(), payload.characters_without_null_termination(), payload.length());

    auto fd = MUST(Core::System::dup(original.fd()));
    auto mapping = MUST(Core::MappedFile::map_from_fd_range_and_close(fd, "anonymous buffer"sv, 0, original.size()));

    EXPECT_EQ(mapping->bytes().slice(0, payload.length()), payload.bytes());
}

TEST_CASE(snapshot_has_independent_contents)
{
    auto original = MUST(Core::AnonymousBuffer::create_with_size(128));

    auto const payload = "copied into private memory"sv;
    memcpy(original.data<void>(), payload.characters_without_null_termination(), payload.length());

    auto snapshot = MUST(original.snapshot());
    EXPECT_EQ(snapshot.size(), original.size());
    EXPECT_EQ(StringView(snapshot.data<char const>(), payload.length()), payload);

    original.data<char>()[0] = 'C';
    EXPECT_EQ(StringView(snapshot.data<char const>(), payload.length()), payload);
}

TEST_CASE(snapshot_rejects_an_invalid_buffer)
{
    EXPECT(Core::AnonymousBuffer {}.snapshot().is_error());
}

#if (defined(AK_OS_LINUX) || defined(AK_OS_FREEBSD)) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK)
TEST_CASE(create_sealable_buffer)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(128, Core::AnonymousBuffer::Sealability::Sealable));
    MUST(Core::System::fcntl(buffer.fd(), F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK));
    auto seals = MUST(Core::System::fcntl(buffer.fd(), F_GET_SEALS, static_cast<uintptr_t>(0)));
    EXPECT_EQ(seals & (F_SEAL_GROW | F_SEAL_SHRINK), F_SEAL_GROW | F_SEAL_SHRINK);
}

TEST_CASE(snapshot_rejects_a_buffer_larger_than_its_backing_store)
{
    auto original = MUST(Core::AnonymousBuffer::create_with_size(64, Core::AnonymousBuffer::Sealability::Sealable));
    auto fd = MUST(Core::System::dup(original.fd()));
    auto oversized = MUST(Core::AnonymousBuffer::create_from_anon_fd(fd, 128));
    EXPECT(oversized.snapshot().is_error());
}
#endif

#ifndef AK_OS_WINDOWS
TEST_CASE(failed_creation_from_an_fd_closes_the_fd)
{
    auto pipe_fds = MUST(Core::System::pipe2(0));
    MUST(Core::System::close(pipe_fds[1]));

    // Pipes cannot be mapped, so creation fails and must close the fd it took ownership of.
    auto result = Core::AnonymousBuffer::create_from_anon_fd(pipe_fds[0], 0x1000);
    EXPECT(result.is_error());
    EXPECT_EQ(fcntl(pipe_fds[0], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
#else
TEST_CASE(failed_creation_from_a_handle_closes_the_handle)
{
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EXPECT(event != nullptr);

    // An event handle cannot back a file mapping, so creation fails and must close the handle it took ownership of.
    auto result = Core::AnonymousBuffer::create_from_anon_fd(to_fd(event), 0x1000);
    EXPECT(result.is_error());

    DWORD handle_flags = 0;
    EXPECT_EQ(GetHandleInformation(event, &handle_flags), 0);
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_INVALID_HANDLE));
}
#endif
