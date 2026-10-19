/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <RustEchoServerEndpoint.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/resource.h>

// NB: Starts the Rust endpoint, which the IPC compiler generated Rust code for from the same definition as the C++ code
//     this test uses, and returns this end of a transport to it.
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
extern "C" void rust_echo_server_serve(mach_port_t receive_right, mach_port_t send_right);

static NonnullOwnPtr<IPC::Transport> start_server()
{
    auto our_receive_right = MUST(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto server_send_right = MUST(our_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));
    auto server_receive_right = MUST(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto our_send_right = MUST(server_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));
    IPC::TransportMachPort::raise_receive_queue_limit(server_receive_right);
    rust_echo_server_serve(server_receive_right.release(), server_send_right.release());
    return make<IPC::TransportMachPort>(move(our_receive_right), move(our_send_right));
}
#else
extern "C" void rust_echo_server_serve(int socket_fd);

static NonnullOwnPtr<IPC::Transport> start_server()
{
    int fds[2] {};
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
    rust_echo_server_serve(fds[1]);
    auto socket = MUST(Core::LocalSocket::adopt_fd(fds[0]));
    MUST(socket->set_blocking(false));
    return make<IPC::TransportSocket>(move(socket));
}
#endif

static void send(IPC::Transport& transport, ErrorOr<IPC::MessageBuffer> encoded)
{
    auto buffer = MUST(move(encoded));
    auto attachments = buffer.take_attachments();
    MUST(transport.post_message(buffer.take_data(), attachments));
}

template<typename Response>
static NonnullOwnPtr<Response> receive(IPC::Transport& transport)
{
    OwnPtr<IPC::Message> message;
    while (!message) {
        transport.wait_until_readable();
        auto should_shutdown = transport.read_as_many_messages_as_possible_without_blocking([&](IPC::Transport::Message&& received) {
            VERIFY(!message);
            message = MUST(RustEchoServerEndpoint::decode_message(received.bytes.bytes(), received.attachments));
        });
        VERIFY(message || should_shutdown == IPC::Transport::ShouldShutdown::No);
    }
    VERIFY(message->message_id() == Response::static_message_id());
    return adopt_own(static_cast<Response&>(*message.leak_ptr()));
}

TEST_CASE(values_make_a_round_trip_through_rust)
{
    auto transport = start_server();
    auto utf16_text = Utf16String::from_utf8("naïve ✓"sv);
    Vector<Optional<i64>> values { 1, {}, -3 };
    auto bytes = MUST(ByteBuffer::copy("bytes"sv.bytes()));
    send(*transport, Messages::RustEchoServer::Echo::static_encode(42, "héllo"sv, utf16_text.utf16_view(), values.span(), bytes.bytes()));

    auto response = receive<Messages::RustEchoServer::EchoResponse>(*transport);
    EXPECT_EQ(response->number(), 42u);
    EXPECT_EQ(response->text(), "héllo"sv);
    EXPECT_EQ(response->utf16_text(), utf16_text);
    EXPECT_EQ(response->values(), values);
    EXPECT_EQ(response->bytes(), bytes);
}

TEST_CASE(strings_with_lone_surrogates_make_a_round_trip_through_rust)
{
    auto transport = start_server();
    auto text = MUST(String::from_utf8("a\xed\xa0\x80z"sv));
    send(*transport, Messages::RustEchoServer::Echo::static_encode(1, text, Utf16View {}, {}, {}));
    auto response = receive<Messages::RustEchoServer::EchoResponse>(*transport);
    EXPECT_EQ(response->text().bytes(), text.bytes());
}

TEST_CASE(large_payloads_make_a_round_trip_through_rust)
{
    auto transport = start_server();
    auto bytes = MUST(ByteBuffer::create_uninitialized(100'000));
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<u8>(i * 7);
    Vector<Optional<i64>> values;
    send(*transport, Messages::RustEchoServer::Echo::static_encode(1, ""sv, Utf16View {}, values.span(), bytes.bytes()));

    auto response = receive<Messages::RustEchoServer::EchoResponse>(*transport);
    EXPECT_EQ(response->bytes(), bytes);
}

TEST_CASE(files_go_to_rust_as_attachments)
{
    auto transport = start_server();
    auto pipe = MUST(Core::System::pipe2(O_CLOEXEC));
    send(*transport, Messages::RustEchoServer::WriteToFile::static_encode(IPC::File::adopt_fd(pipe[1]), "from rust"sv));
    (void)receive<Messages::RustEchoServer::WriteToFileResponse>(*transport);

    char buffer[32] = {};
    auto read = MUST(Core::System::read(pipe[0], { buffer, sizeof(buffer) }));
    EXPECT_EQ(StringView(buffer, read), "from rust"sv);
    MUST(Core::System::close(pipe[0]));
}

// NB: Linux fails sending descriptors once all those the user has in flight exceed the sender's limit, which other
//     processes running at the same time count towards, so raise it as LibMain does for every process.
static void raise_descriptor_limit()
{
    rlimit limit {};
    VERIFY(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    limit.rlim_cur = min<rlim_t>(limit.rlim_max, 65536);
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    // NB: macOS rejects a descriptor limit above OPEN_MAX, but still needs one above its default of 256.
    limit.rlim_cur = min<rlim_t>(limit.rlim_cur, OPEN_MAX);
#endif
    VERIFY(setrlimit(RLIMIT_NOFILE, &limit) == 0);
}

TEST_CASE(many_files_make_a_round_trip_through_rust)
{
    // NB: More files than one sendmsg() carries, so that on sockets some go ahead of the message.
    static constexpr size_t file_count = 200;
    raise_descriptor_limit();
    auto transport = start_server();
    Vector<int> readers;
    Vector<IPC::File> files;
    for (size_t i = 0; i < file_count; ++i) {
        auto pipe = MUST(Core::System::pipe2(O_CLOEXEC));
        readers.append(pipe[0]);
        files.append(IPC::File::adopt_fd(pipe[1]));
    }
    send(*transport, Messages::RustEchoServer::EchoFiles::static_encode(files));

    auto returned_files = receive<Messages::RustEchoServer::EchoFilesResponse>(*transport)->take_files();
    EXPECT_EQ(returned_files.size(), file_count);
    for (size_t i = 0; i < min(returned_files.size(), file_count); ++i) {
        auto fd = returned_files[i].take_fd();
        MUST(Core::System::write(fd, "!"sv.bytes()));
        MUST(Core::System::close(fd));
        char buffer = 0;
        EXPECT_EQ(MUST(Core::System::read(readers[i], { &buffer, 1 })), 1uz);
        EXPECT_EQ(buffer, '!');
    }
    for (auto reader : readers)
        MUST(Core::System::close(reader));
}

TEST_CASE(the_rust_end_closing_ends_the_connection)
{
    auto transport = start_server();
    send(*transport, Messages::RustEchoServer::Stop::static_encode());
    auto should_shutdown = IPC::Transport::ShouldShutdown::No;
    while (should_shutdown == IPC::Transport::ShouldShutdown::No) {
        transport->wait_until_readable();
        should_shutdown = transport->read_as_many_messages_as_possible_without_blocking([](auto&&) { VERIFY_NOT_REACHED(); });
    }
}

TEST_CASE(utf8_attributes_match_cpp_validation)
{
    for (auto text : { ""sv, "héllo"sv, "\xed\xa0\x80\xed\xbf\xbf"sv }) {
        auto transport = start_server();
        send(*transport, Messages::RustEchoServer::EchoUtf8::static_encode(text, text.bytes()));
        auto response = receive<Messages::RustEchoServer::EchoUtf8Response>(*transport);
        EXPECT_EQ(response->text(), text);
        EXPECT_EQ(response->bytes().bytes(), text.bytes());
    }
    for (auto text : { "\xff"sv, "\xc0\x80"sv, "\xe2\x82"sv, "\xed\xa0\x80\xff"sv, "\xf4\x90\x80\x80"sv }) {
        for (auto invalid_text : { true, false }) {
            auto string = invalid_text ? text : ""sv;
            auto bytes = invalid_text ? ""sv.bytes() : text.bytes();
            auto buffer = MUST(Messages::RustEchoServer::EchoUtf8::static_encode(string, bytes));
            Queue<IPC::Attachment> attachments;
            EXPECT(RustEchoServerEndpoint::decode_message(buffer.data(), attachments).is_error());
            auto response = MUST(Messages::RustEchoServer::EchoUtf8Response::static_encode(string, bytes));
            EXPECT(RustEchoServerEndpoint::decode_message(response.data(), attachments).is_error());
        }
    }
}

TEST_CASE(cpp_decoder_accepts_unused_attachments)
{
    auto buffer = MUST(Messages::RustEchoServer::Stop::static_encode());
    auto pipe = MUST(Core::System::pipe2(O_CLOEXEC));
    Queue<IPC::Attachment> attachments;
    attachments.enqueue(IPC::Attachment::from_fd(pipe[1]));
    EXPECT(!RustEchoServerEndpoint::decode_message(buffer.data(), attachments).is_error());
    EXPECT_EQ(attachments.size(), 1u);
    MUST(Core::System::close(pipe[0]));
}

TEST_CASE(cpp_and_rust_decoders_accept_trailing_bytes)
{
    auto buffer = MUST(Messages::RustEchoServer::EchoUtf8::static_encode("hello"sv, {}));
    auto bytes = buffer.take_data();
    MUST(bytes.try_append(0));
    Queue<IPC::Attachment> attachments;
    EXPECT(!RustEchoServerEndpoint::decode_message(bytes, attachments).is_error());
    auto transport = start_server();
    Vector<IPC::Attachment> outgoing_attachments;
    MUST(transport->post_message(move(bytes), outgoing_attachments));
    auto response = receive<Messages::RustEchoServer::EchoUtf8Response>(*transport);
    EXPECT_EQ(response->text(), "hello"sv);
}
