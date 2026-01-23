/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>
#include <LibMediaServerClient/Client.h>
#include <LibTest/TestSuite.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef AK_OS_WINDOWS
#    include <LibThreading/Thread.h>
#endif

static ErrorOr<ByteString> find_mediaserver_executable_path()
{
    auto current_executable_path = TRY(Core::System::current_executable_path());

    LexicalPath current_executable_lexical_path(current_executable_path);
    auto current_dir = current_executable_lexical_path.dirname();

    auto media_server_path = LexicalPath::join(current_dir, ".."sv, "libexec"sv, "MediaServer"sv).string();
    media_server_path = LexicalPath::canonicalized_path(move(media_server_path));

    TRY(Core::System::access(media_server_path.view(), X_OK));
    return media_server_path;
}

static ErrorOr<Core::Process> spawn_mediaserver_with_takeover(int takeover_fd)
{
    auto media_server_path = TRY(find_mediaserver_executable_path());

    auto takeover_string = ByteString::formatted("smoke:{}", takeover_fd);
    TRY(Core::Environment::set("SOCKET_TAKEOVER"sv, takeover_string, Core::Environment::Overwrite::Yes));

    Vector<ByteString> child_arguments;

    Core::ProcessSpawnOptions options {
        .name = "MediaServer"sv,
        .executable = media_server_path,
        .search_for_executable_in_path = false,
        .arguments = child_arguments,
    };

    auto media_server_process = TRY(Core::Process::spawn(options));
    TRY(Core::Environment::unset("SOCKET_TAKEOVER"sv));

    return media_server_process;
}

TEST_CASE(mediaserver_smoke_ipc_and_shared_ring_buffer)
{
    srand(0);

    Core::EventLoop event_loop;

    constexpr size_t capacity = 4096;

    int fds[2] {};
    EXPECT(!Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds).is_error());

    auto media_server_process_or_error = spawn_mediaserver_with_takeover(fds[1]);
    EXPECT(!media_server_process_or_error.is_error());
    if (media_server_process_or_error.is_error()) {
        (void)Core::System::close(fds[0]);
        (void)Core::System::close(fds[1]);
        return;
    }

    auto media_server_process = media_server_process_or_error.release_value();

    auto kill_media_server = AK::ArmedScopeGuard([&] {
        (void)Core::System::kill(media_server_process.pid(), SIGTERM);
        (void)media_server_process.wait_for_termination();
    });

    EXPECT(!Core::System::close(fds[1]).is_error());

    auto socket_or_error = Core::LocalSocket::adopt_fd(fds[0]);
    EXPECT(!socket_or_error.is_error());
    if (socket_or_error.is_error())
        return;

    auto transport = make<IPC::Transport>(socket_or_error.release_value());
    auto client = adopt_ref(*new MediaServerClient::Client(move(transport)));

    auto ring_buffer_or_error = client->create_shared_single_producer_circular_buffer(capacity);
    EXPECT(!ring_buffer_or_error.is_error());
    if (ring_buffer_or_error.is_error())
        return;

    auto ring_buffer = ring_buffer_or_error.release_value();
    EXPECT_EQ(ring_buffer.capacity(), capacity);

    Vector<u8> pending;
    pending.ensure_capacity(capacity * 2);

    for (size_t iteration = 0; iteration < 10'000; ++iteration) {
        size_t write_size = static_cast<size_t>(get_random<u32>() % 1024);
        Vector<u8> write_data;
        write_data.resize(write_size);
        for (size_t i = 0; i < write_data.size(); ++i)
            write_data[i] = get_random<u8>();

        auto written = ring_buffer.try_write(write_data.span());
        EXPECT(written <= write_data.size());
        for (size_t i = 0; i < written; ++i)
            pending.append(write_data[i]);

        size_t read_size = static_cast<size_t>(get_random<u32>() % 1024);
        Vector<u8> read_data;
        read_data.resize(read_size);

        auto read = ring_buffer.try_read(read_data.span());
        EXPECT(read <= read_data.size());
        EXPECT(read <= pending.size());

        for (size_t i = 0; i < read; ++i)
            EXPECT_EQ(read_data[i], pending[i]);

        if (read != 0)
            pending.remove(0, read);
    }

    Vector<u8> drain;
    drain.resize(capacity * 2);
    while (!pending.is_empty()) {
        auto read = ring_buffer.try_read(drain.span());
        EXPECT(read <= pending.size());

        for (size_t i = 0; i < read; ++i)
            EXPECT_EQ(drain[i], pending[i]);

        if (read != 0)
            pending.remove(0, read);
    }

    kill_media_server.disarm();
    EXPECT(!Core::System::kill(media_server_process.pid(), SIGTERM).is_error());
    EXPECT(media_server_process.wait_for_termination().is_error() == false);
}

#ifndef AK_OS_WINDOWS
TEST_CASE(mediaserver_smoke_shared_ring_buffer_two_threads_ordered_u32)
{
    srand(0);

    Core::EventLoop event_loop;

    constexpr size_t capacity = 1 << 16;
    constexpr u32 iterations = 50'000;

    int fds[2] {};
    EXPECT(!Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds).is_error());

    auto media_server_process_or_error = spawn_mediaserver_with_takeover(fds[1]);
    EXPECT(!media_server_process_or_error.is_error());
    if (media_server_process_or_error.is_error()) {
        (void)Core::System::close(fds[0]);
        (void)Core::System::close(fds[1]);
        return;
    }

    auto media_server_process = media_server_process_or_error.release_value();

    auto kill_media_server = AK::ArmedScopeGuard([&] {
        (void)Core::System::kill(media_server_process.pid(), SIGTERM);
        (void)media_server_process.wait_for_termination();
    });

    EXPECT(!Core::System::close(fds[1]).is_error());

    auto socket_or_error = Core::LocalSocket::adopt_fd(fds[0]);
    EXPECT(!socket_or_error.is_error());
    if (socket_or_error.is_error())
        return;

    auto transport = make<IPC::Transport>(socket_or_error.release_value());
    auto client = adopt_ref(*new MediaServerClient::Client(move(transport)));

    auto ring_buffer_or_error = client->create_shared_single_producer_circular_buffer(capacity);
    EXPECT(!ring_buffer_or_error.is_error());
    if (ring_buffer_or_error.is_error())
        return;

    auto ring_buffer = ring_buffer_or_error.release_value();
    EXPECT_EQ(ring_buffer.capacity(), capacity);

    Atomic<bool> done { false };
    Atomic<bool> failed { false };

    auto consumer = MUST(Threading::Thread::try_create([&]() -> intptr_t {
        u32 expected = 0;
        Array<u8, sizeof(u32)> tmp;
        while (expected < iterations) {
            if (ring_buffer.try_read(tmp.span()) != sizeof(u32)) {
                usleep(0);
                continue;
            }

            u32 value = 0;
            __builtin_memcpy(&value, tmp.data(), sizeof(u32));
            if (value != expected) {
                failed.store(true, AK::MemoryOrder::memory_order_release);
                break;
            }
            ++expected;
        }

        done.store(true, AK::MemoryOrder::memory_order_release);
        return 0;
    },
        "MediaServer SPSC consumer"sv));

    auto producer = MUST(Threading::Thread::try_create([&]() -> intptr_t {
        for (u32 i = 0; i < iterations; ++i) {
            Array<u8, sizeof(u32)> tmp;
            __builtin_memcpy(tmp.data(), &i, sizeof(u32));
            while (ring_buffer.try_write(tmp.span()) != sizeof(u32))
                usleep(0);
            if (failed.load(AK::MemoryOrder::memory_order_acquire))
                break;
        }
        return 0;
    },
        "MediaServer SPSC producer"sv));

    producer->start();
    consumer->start();

    (void)producer->join();
    (void)consumer->join();

    EXPECT(done.load(AK::MemoryOrder::memory_order_acquire));
    EXPECT(!failed.load(AK::MemoryOrder::memory_order_acquire));

    kill_media_server.disarm();
    EXPECT(!Core::System::kill(media_server_process.pid(), SIGTERM).is_error());
    EXPECT(media_server_process.wait_for_termination().is_error() == false);
}
#endif
