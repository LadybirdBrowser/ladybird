/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/MaybeOwned.h>
#include <AK/String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/LocalServer.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibCore/UDPServer.h>
#include <LibTest/TestCase.h>
#include <LibThreading/BackgroundAction.h>
#include <fcntl.h>
#include <unistd.h>

// File tests

TEST_CASE(file_open)
{
    auto maybe_file = Core::File::open("/tmp/file-open-test.txt"sv, Core::File::OpenMode::Write);
    if (maybe_file.is_error()) {
        warnln("Failed to open the file: {}", strerror(maybe_file.error().code()));
        VERIFY_NOT_REACHED();
    }

    // Testing out some basic file properties.
    auto file = maybe_file.release_value();
    EXPECT(file->is_open());
    EXPECT(!file->is_eof());

    auto size = TRY_OR_FAIL(file->size());
    EXPECT_EQ(size, 0ul);
}

TEST_CASE(file_write_bytes)
{
    auto file = TRY_OR_FAIL(Core::File::open("/tmp/file-write-bytes-test.txt"sv, Core::File::OpenMode::Write));

    constexpr auto some_words = "These are some words"sv;
    ReadonlyBytes buffer { some_words.characters_without_null_termination(), some_words.length() };
    TRY_OR_FAIL(file->write_some(buffer));
}

constexpr auto expected_buffer_contents = "&lt;small&gt;(Please consider translating this message for the benefit of your fellow Wikimedians. Please also consider translating"sv;

TEST_CASE(file_read_bytes)
{
    auto file = TRY_OR_FAIL(Core::File::open("./long_lines.txt"sv, Core::File::OpenMode::Read));

    auto buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(131));

    auto result = file->read_some(buffer);
    EXPECT_EQ(TRY_OR_FAIL(result).size(), 131ul);

    StringView buffer_contents { buffer.bytes() };
    EXPECT_EQ(buffer_contents, expected_buffer_contents);
}

constexpr auto expected_seek_contents1 = "|Lleer esti mens"sv;
constexpr auto expected_seek_contents2 = "s of advanced ad"sv;
constexpr auto expected_seek_contents3 = "levels of advanc"sv;

TEST_CASE(file_seeking_around)
{
    auto file = TRY_OR_FAIL(Core::File::open("./long_lines.txt"sv, Core::File::OpenMode::Read));

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
    auto file = TRY_OR_FAIL(Core::File::open("./10kb.txt"sv, Core::File::OpenMode::Read));
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

TEST_CASE(file_buffered_write_and_seek)
{
    auto file = TRY_OR_FAIL(Core::OutputBufferedFile::create(TRY_OR_FAIL(Core::File::open("/tmp/file-buffered-write-test.txt"sv, Core::File::OpenMode::Truncate | Core::File::OpenMode::ReadWrite))));

    TRY_OR_FAIL(file->write_some("0123456789"sv.bytes()));
    EXPECT_EQ(file->tell().release_value(), 10ul);

    // Reads don't go through the buffer, so after we seek, the data must be available from the underlying file.
    TRY_OR_FAIL(file->seek(0, AK::SeekMode::SetPosition));
    auto first_byte = TRY_OR_FAIL(file->read_value<u8>());
    EXPECT_EQ(first_byte, static_cast<u8>('0'));

    TRY_OR_FAIL(file->seek(9, AK::SeekMode::SetPosition));
    auto last_byte = TRY_OR_FAIL(file->read_value<u8>());
    EXPECT_EQ(last_byte, static_cast<u8>('9'));
    EXPECT_EQ(file->tell().release_value(), 10ul);
}

TEST_CASE(file_adopt_fd)
{
    int rc = ::open("./long_lines.txt", O_RDONLY);
    EXPECT(rc >= 0);

    auto file = TRY_OR_FAIL(Core::File::adopt_fd(rc, Core::File::OpenMode::Read));

    EXPECT_EQ(file->size().release_value(), 8702ul);

    auto buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(16));

    StringView buffer_contents { buffer.bytes() };

    TRY_OR_FAIL(file->seek(500, SeekMode::SetPosition));
    EXPECT_EQ(file->tell().release_value(), 500ul);
    TRY_OR_FAIL(file->read_until_filled(buffer));
    EXPECT_EQ(buffer_contents, expected_seek_contents1);

    // A single seek & read test should be fine for now.
}

TEST_CASE(file_adopt_invalid_fd)
{
    auto maybe_file = Core::File::adopt_fd(-1, Core::File::OpenMode::Read);
    EXPECT(maybe_file.is_error());
    EXPECT_EQ(maybe_file.error().code(), EBADF);
}

TEST_CASE(file_truncate)
{
    auto file = TRY_OR_FAIL(Core::File::open("/tmp/file-truncate-test.txt"sv, Core::File::OpenMode::Write));

    TRY_OR_FAIL(file->truncate(999));
    EXPECT_EQ(file->size().release_value(), 999ul);

    TRY_OR_FAIL(file->truncate(42));
    EXPECT_EQ(file->size().release_value(), 42ul);
}

// TCPSocket tests

TEST_CASE(should_error_when_connection_fails)
{
    // NOTE: This is required here because Core::TCPSocket requires
    //       Core::EventLoop through Core::Notifier.
    Core::EventLoop event_loop;

    auto maybe_tcp_socket = Core::TCPSocket::connect({ { 127, 0, 0, 1 }, 1234 });
    EXPECT(maybe_tcp_socket.is_error());
    EXPECT(maybe_tcp_socket.error().is_errno());
    EXPECT(maybe_tcp_socket.error().code() == ECONNREFUSED);
}

constexpr auto sent_data = "Mr. Watson, come here. I want to see you."sv;

TEST_CASE(tcp_socket_read)
{
    // NOTE: This is required here because Core::TCPServer requires
    //       Core::EventLoop through Core::Notifier.
    Core::EventLoop event_loop;

    auto tcp_server = TRY_OR_FAIL(Core::TCPServer::try_create());
    TRY_OR_FAIL(tcp_server->listen({ 127, 0, 0, 1 }, 9090, Core::TCPServer::AllowAddressReuse::Yes));
    TRY_OR_FAIL(tcp_server->set_blocking(true));

    auto client_socket = TRY_OR_FAIL(Core::TCPSocket::connect({ { 127, 0, 0, 1 }, 9090 }));

    EXPECT(client_socket->is_open());

    auto server_socket = TRY_OR_FAIL(tcp_server->accept());
    TRY_OR_FAIL(server_socket->write_some({ sent_data.characters_without_null_termination(), sent_data.length() }));
    server_socket->close();

    EXPECT(client_socket->can_read_without_blocking(100).release_value());
    EXPECT_EQ(client_socket->pending_bytes().release_value(), sent_data.length());

    auto receive_buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(64));
    auto read_bytes = TRY_OR_FAIL(client_socket->read_some(receive_buffer));

    StringView received_data { read_bytes };
    EXPECT_EQ(sent_data, received_data);
}

TEST_CASE(tcp_socket_write)
{
    Core::EventLoop event_loop;

    auto tcp_server = TRY_OR_FAIL(Core::TCPServer::try_create());
    TRY_OR_FAIL(tcp_server->listen({ 127, 0, 0, 1 }, 9090, Core::TCPServer::AllowAddressReuse::Yes));
    TRY_OR_FAIL(tcp_server->set_blocking(true));

    auto client_socket = TRY_OR_FAIL(Core::TCPSocket::connect({ { 127, 0, 0, 1 }, 9090 }));

    auto server_socket = TRY_OR_FAIL(tcp_server->accept());
    TRY_OR_FAIL(server_socket->set_blocking(true));

    TRY_OR_FAIL(client_socket->write_until_depleted({ sent_data.characters_without_null_termination(), sent_data.length() }));
    client_socket->close();

    auto receive_buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(64));
    auto read_bytes = TRY_OR_FAIL(server_socket->read_some(receive_buffer));

    StringView received_data { read_bytes };
    EXPECT_EQ(sent_data, received_data);
}

TEST_CASE(tcp_socket_eof)
{
    Core::EventLoop event_loop;

    auto tcp_server = TRY_OR_FAIL(Core::TCPServer::try_create());
    TRY_OR_FAIL(tcp_server->listen({ 127, 0, 0, 1 }, 9090, Core::TCPServer::AllowAddressReuse::Yes));
    TRY_OR_FAIL(tcp_server->set_blocking(true));

    auto client_socket = TRY_OR_FAIL(Core::TCPSocket::connect({ { 127, 0, 0, 1 }, 9090 }));

    EXPECT(client_socket->is_open());

    auto server_socket = tcp_server->accept().release_value();
    server_socket->close();

    // NOTE: This may seem unintuitive, but poll will mark a fd which has
    //       reached EOF (i.e. in the case of the other side disconnecting) as
    //       POLLIN.
    EXPECT(client_socket->can_read_without_blocking(100).release_value());
    EXPECT_EQ(client_socket->pending_bytes().release_value(), 0ul);

    auto receive_buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(1));
    EXPECT(client_socket->read_some(receive_buffer).release_value().is_empty());
    EXPECT(client_socket->is_eof());
}

// UDPSocket tests

constexpr auto udp_reply_data = "Well hello friends!"sv;

TEST_CASE(udp_socket_read_write)
{
    // NOTE: This is required here because Core::UDPServer requires
    //       Core::EventLoop through Core::Notifier.
    Core::EventLoop event_loop;

    auto udp_server = Core::UDPServer::construct();
    EXPECT(udp_server->bind({ 127, 0, 0, 1 }, 9090));

    auto client_socket = TRY_OR_FAIL(Core::UDPSocket::connect({ { 127, 0, 0, 1 }, 9090 }));

    EXPECT(client_socket->is_open());
    TRY_OR_FAIL(client_socket->write_until_depleted({ sent_data.characters_without_null_termination(), sent_data.length() }));

    // FIXME: UDPServer::receive sadly doesn't give us a way to block on it,
    // currently.
    usleep(100000);

    struct sockaddr_in client_address;
    auto server_receive_buffer = TRY_OR_FAIL(udp_server->receive(64, client_address));
    EXPECT(!server_receive_buffer.is_empty());

    StringView server_received_data { server_receive_buffer.bytes() };
    EXPECT_EQ(server_received_data, sent_data);

    TRY_OR_FAIL(udp_server->send({ udp_reply_data.characters_without_null_termination(), udp_reply_data.length() }, client_address));

    EXPECT(client_socket->can_read_without_blocking(100).release_value());
#ifdef AK_OS_LINUX
    // FIONREAD only returns this expected 'the bytes I just sent' value on Linux. macOS gives a completely different value.
    EXPECT_EQ(client_socket->pending_bytes().release_value(), udp_reply_data.length());
#endif

    // Testing that supplying a smaller buffer than required causes a failure.
    auto small_buffer = ByteBuffer::create_uninitialized(8).release_value();
    EXPECT_EQ(client_socket->read_some(small_buffer).error().code(), EMSGSIZE);

    auto client_receive_buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(64));
    auto read_bytes = TRY_OR_FAIL(client_socket->read_some(client_receive_buffer));

    StringView client_received_data { read_bytes };
    EXPECT_EQ(udp_reply_data, client_received_data);
}

// LocalSocket tests

TEST_CASE(local_socket_read)
{
    Core::EventLoop event_loop;

    auto local_server = Core::LocalServer::construct();
    EXPECT(local_server->listen("/tmp/test-socket"));

    local_server->on_accept = [&](NonnullOwnPtr<Core::LocalSocket> server_socket) {
        TRY_OR_FAIL(server_socket->write_some(sent_data.bytes()));

        event_loop.quit(0);
        event_loop.pump();
    };

    // NOTE: Doing this on another thread, because otherwise we're at an
    //       impasse. LocalSocket::connect blocks because there's nobody to
    //       accept, and LocalServer::accept blocks because there's nobody
    //       connected.
    auto background_action = Threading::BackgroundAction<int>::construct(
        [](auto&) {
            Core::EventLoop event_loop;

            auto client_socket = MUST(Core::LocalSocket::connect("/tmp/test-socket"));

            EXPECT(client_socket->is_open());

            EXPECT(client_socket->can_read_without_blocking(100).release_value());
            EXPECT_EQ(client_socket->pending_bytes().release_value(), sent_data.length());

            auto receive_buffer = MUST(ByteBuffer::create_uninitialized(64));
            auto read_bytes = MUST(client_socket->read_some(receive_buffer));

            StringView received_data { read_bytes };
            EXPECT_EQ(sent_data, received_data);

            return 0;
        },
        nullptr);

    event_loop.exec();
    ::unlink("/tmp/test-socket");
}

TEST_CASE(local_socket_write)
{
    Core::EventLoop event_loop;

    auto local_server = Core::LocalServer::construct();
    EXPECT(local_server->listen("/tmp/test-socket"));

    local_server->on_accept = [&](NonnullOwnPtr<Core::LocalSocket> server_socket) {
        // NOTE: For some reason LocalServer gives us a nonblocking socket..?
        MUST(server_socket->set_blocking(true));

        EXPECT(MUST(server_socket->can_read_without_blocking(100)));
        auto pending_bytes = MUST(server_socket->pending_bytes());
        auto receive_buffer = TRY_OR_FAIL(ByteBuffer::create_uninitialized(pending_bytes));
        auto read_bytes = TRY_OR_FAIL(server_socket->read_some(receive_buffer));
        EXPECT_EQ(read_bytes.size(), sent_data.length());

        StringView received_data { read_bytes };
        EXPECT_EQ(sent_data, received_data);

        event_loop.quit(0);
        event_loop.pump();
    };

    // NOTE: Same reason as in the local_socket_read test.
    auto background_action = Threading::BackgroundAction<int>::construct(
        [](auto&) {
            auto client_socket = MUST(Core::LocalSocket::connect("/tmp/test-socket"));

            MUST(client_socket->write_until_depleted({ sent_data.characters_without_null_termination(), sent_data.length() }));
            client_socket->close();

            return 0;
        },
        nullptr);

    event_loop.exec();
    ::unlink("/tmp/test-socket");
}

// Buffered stream tests

TEST_CASE(buffered_long_file_read)
{
    auto raw_file = TRY_OR_FAIL(Core::File::open("./long_lines.txt"sv, Core::File::OpenMode::Read));
    auto file = TRY_OR_FAIL(Core::InputBufferedFile::create(move(raw_file)));

    auto buffer = ByteBuffer::create_uninitialized(4096).release_value();
    TRY_OR_FAIL(file->seek(255, SeekMode::SetPosition));
    EXPECT(file->can_read_line().release_value());
    auto line = TRY_OR_FAIL(file->read_line(buffer));
    EXPECT_EQ(line.length(), 4095ul); // 4095 bytes on the third line

    // Testing that buffering with seeking works properly
    TRY_OR_FAIL(file->seek(365, SeekMode::SetPosition));
    auto after_seek_line = TRY_OR_FAIL(file->read_line(buffer));
    EXPECT_EQ(after_seek_line.length(), 3985ul); // 4095 - 110
}

TEST_CASE(buffered_small_file_read)
{
    auto raw_file = TRY_OR_FAIL(Core::File::open("./small.txt"sv, Core::File::OpenMode::Read));
    auto file = TRY_OR_FAIL(Core::InputBufferedFile::create(move(raw_file)));

    static constexpr StringView expected_lines[] {
        "Well"sv,
        "hello"sv,
        "friends!"sv,
        ":^)"sv
    };

    // Testing that we don't read out of bounds when the entire file fits into the buffer
    auto buffer = ByteBuffer::create_uninitialized(4096).release_value();
    for (auto const& line : expected_lines) {
        VERIFY(file->can_read_line().release_value());
        auto read_line = TRY_OR_FAIL(file->read_line(buffer));
        EXPECT_EQ(read_line.length(), line.length());
        EXPECT_EQ(StringView(buffer.span().trim(read_line.length())), line);
    }
    bool can_read_line = TRY_OR_FAIL(file->can_read_line());
    EXPECT(!can_read_line);
}

TEST_CASE(buffered_file_tell_and_seek)
{
    // We choose a buffer size of 12 bytes to cover half of the input file.
    auto file = Core::File::open("./small.txt"sv, Core::File::OpenMode::Read).release_value();
    auto buffered_file = Core::InputBufferedFile::create(move(file), 12).release_value();

    // Initial state.
    {
        auto current_offset = buffered_file->tell().release_value();
        EXPECT_EQ(current_offset, 0ul);
    }

    // Read a character.
    {
        auto character = buffered_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'W');
        auto current_offset = buffered_file->tell().release_value();
        EXPECT_EQ(current_offset, 1ul);
    }

    // Read one more character.
    {
        auto character = buffered_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'e');
        auto current_offset = buffered_file->tell().release_value();
        EXPECT_EQ(current_offset, 2ul);
    }

    // Seek seven characters forward.
    {
        auto current_offset = buffered_file->seek(7, SeekMode::FromCurrentPosition).release_value();
        EXPECT_EQ(current_offset, 9ul);
    }

    // Read a character again.
    {
        auto character = buffered_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'o');
        auto current_offset = buffered_file->tell().release_value();
        EXPECT_EQ(current_offset, 10ul);
    }

    // Seek five characters backwards.
    {
        auto current_offset = buffered_file->seek(-5, SeekMode::FromCurrentPosition).release_value();
        EXPECT_EQ(current_offset, 5ul);
    }

    // Read a character.
    {
        auto character = buffered_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'h');
        auto current_offset = buffered_file->tell().release_value();
        EXPECT_EQ(current_offset, 6ul);
    }

    // Seek back to the beginning.
    {
        auto current_offset = buffered_file->seek(0, SeekMode::SetPosition).release_value();
        EXPECT_EQ(current_offset, 0ul);
    }

    // Read the first character. This should prime the buffer if it hasn't happened already.
    {
        auto character = buffered_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'W');
        auto current_offset = buffered_file->tell().release_value();
        EXPECT_EQ(current_offset, 1ul);
    }

    // Seek beyond the buffer size, which should invalidate the buffer.
    {
        auto current_offset = buffered_file->seek(12, SeekMode::SetPosition).release_value();
        EXPECT_EQ(current_offset, 12ul);
    }

    // Ensure that we still read the correct contents from the new offset with a (presumably) freshly filled buffer.
    {
        auto character = buffered_file->read_value<char>().release_value();
        EXPECT_EQ(character, 'r');
        auto current_offset = buffered_file->tell().release_value();
        EXPECT_EQ(current_offset, 13ul);
    }
}

constexpr auto new_newlines_message = "Hi, look, no newlines"sv;

TEST_CASE(buffered_file_without_newlines)
{
    constexpr auto filename = "/tmp/file-without-newlines"sv;
    auto file_wo_newlines = Core::File::open(filename, Core::File::OpenMode::Write).release_value();
    TRY_OR_FAIL(file_wo_newlines->write_until_depleted(new_newlines_message.bytes()));
    file_wo_newlines->close();

    auto ro_file = Core::InputBufferedFile::create(Core::File::open(filename, Core::File::OpenMode::Read).release_value(), new_newlines_message.length() + 1).release_value();

    auto can_read_line = TRY_OR_FAIL(ro_file->can_read_line());
    EXPECT(can_read_line);
    auto can_read_up_to_newline = TRY_OR_FAIL(ro_file->can_read_up_to_delimiter("\n"sv.bytes()));
    EXPECT(!can_read_up_to_newline);
    Array<u8, new_newlines_message.length() + 1> buffer;
    EXPECT(ro_file->read_line(buffer).release_value() == new_newlines_message);
}

constexpr auto buffered_sent_data = "Well hello friends!\n:^)\nThis shouldn't be present. :^("sv;
constexpr auto first_line = "Well hello friends!"sv;
constexpr auto second_line = ":^)"sv;

TEST_CASE(buffered_tcp_socket_read)
{
    Core::EventLoop event_loop;

    auto tcp_server = TRY_OR_FAIL(Core::TCPServer::try_create());
    TRY_OR_FAIL(tcp_server->listen({ 127, 0, 0, 1 }, 9090, Core::TCPServer::AllowAddressReuse::Yes));
    TRY_OR_FAIL(tcp_server->set_blocking(true));

    auto unbuffered_socket = TRY_OR_FAIL(Core::TCPSocket::connect({ { 127, 0, 0, 1 }, 9090 }));
    auto client_socket = TRY_OR_FAIL(Core::BufferedTCPSocket::create(move(unbuffered_socket)));

    EXPECT(client_socket->is_open());

    auto server_socket = TRY_OR_FAIL(tcp_server->accept());
    TRY_OR_FAIL(server_socket->write_some({ buffered_sent_data.characters_without_null_termination(), sent_data.length() }));

    EXPECT(client_socket->can_read_without_blocking(100).release_value());

    auto receive_buffer = ByteBuffer::create_uninitialized(64).release_value();

    auto first_received_line = TRY_OR_FAIL(client_socket->read_line(receive_buffer));
    EXPECT_EQ(first_received_line, first_line);

    auto second_received_line = TRY_OR_FAIL(client_socket->read_line(receive_buffer));
    EXPECT_EQ(second_received_line, second_line);
}
