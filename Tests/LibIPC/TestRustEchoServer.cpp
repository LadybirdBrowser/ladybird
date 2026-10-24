/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// NB: The generated header comes first, so that it declares its codecs before anything else in LibIPC is seen.
#include <RustEchoServerEndpoint.h>

#include <AK/Function.h>
#include <AK/MemoryStream.h>
#include <LibCore/System.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <limits.h>
#include <math.h>
#if !defined(AK_OS_WINDOWS)
#    include <fcntl.h>
#    include <sys/resource.h>
#endif

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
#elif defined(AK_OS_WINDOWS)
extern "C" void rust_echo_server_serve(void* pipe);

static NonnullOwnPtr<IPC::Transport> start_server()
{
    auto handles = MUST(Core::System::duplex_pipe());
    rust_echo_server_serve(to_handle(handles[1]));
    return MUST(IPC::TransportWindows::adopt_fd(handles[0]));
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

// NB: The Rust transport on Windows does not carry attachments yet, which go through the handle broker there.
#if !defined(AK_OS_WINDOWS)
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
#    if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    // NB: macOS rejects a descriptor limit above OPEN_MAX, but still needs one above its default of 256.
    limit.rlim_cur = min<rlim_t>(limit.rlim_cur, OPEN_MAX);
#    endif
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

#endif

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

#if !defined(AK_OS_WINDOWS)
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
#endif

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

static RustEcho::Record make_record()
{
    return RustEcho::Record {
        .id = 7,
        .label = "label"_string,
        .name = "name"sv,
        .bytes = MUST(ByteBuffer::copy("bytes"sv.bytes())),
        .values = { 1, 2, 3 },
        .scale = 1.5,
        .shape = RustEcho::Shape::Square,
    };
}

static IPC::MessageDataType encode_value(RustEcho::Value const& value)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(value));
    return buffer.take_data();
}

static void expect_refused_by_cpp_and_rust(ReadonlyBytes message)
{
    Queue<IPC::Attachment> attachments;
    EXPECT(RustEchoServerEndpoint::decode_message(message, attachments).is_error());

    auto transport = start_server();
    Vector<IPC::Attachment> outgoing_attachments;
    IPC::MessageDataType bytes;
    bytes.append(message.data(), message.size());
    MUST(transport->post_message(move(bytes), outgoing_attachments));
    // NB: A valid message follows, so the Rust end either closes the connection or answers it, and never leaves this
    //     waiting.
    send(*transport, Messages::RustEchoServer::EchoValue::static_encode(RustEcho::Value { 1u }));
    auto answered = false;
    auto should_shutdown = IPC::Transport::ShouldShutdown::No;
    while (!answered && should_shutdown == IPC::Transport::ShouldShutdown::No) {
        transport->wait_until_readable();
        should_shutdown = transport->read_as_many_messages_as_possible_without_blocking([&](auto&&) { answered = true; });
    }
    if (answered)
        FAIL("The Rust endpoint answered a message after one it should have refused");
}

// NB: Encodes a message with a record field by field, past the generated encoder, which refuses invalid records.
static IPC::MessageDataType encode_record_message_unchecked(RustEcho::Record const& record)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(Messages::RustEchoServer::EchoValue::ENDPOINT_MAGIC));
    MUST(encoder.encode(Messages::RustEchoServer::EchoValue::static_message_id()));
    MUST(encoder.encode<u8>(0));
    MUST(encoder.encode(record.id));
    MUST(encoder.encode(record.label));
    MUST(encoder.encode(record.name));
    MUST(encoder.encode(record.bytes));
    MUST(encoder.encode(record.values));
    MUST(encoder.encode(record.scale));
    MUST(encoder.encode(record.shape));
    return buffer.take_data();
}

TEST_CASE(declared_types_make_a_round_trip_through_rust)
{
    auto transport = start_server();
    for (RustEcho::Value value : { RustEcho::Value { make_record() }, RustEcho::Value { 42u }, RustEcho::Value { RustEcho::Shape::Circle } }) {
        send(*transport, Messages::RustEchoServer::EchoValue::static_encode(value));
        auto response = receive<Messages::RustEchoServer::EchoValueResponse>(*transport);
        EXPECT_EQ(encode_value(response->value()), encode_value(value));
    }
}

TEST_CASE(declared_types_are_checked_by_cpp_and_rust)
{
    EXPECT_EQ(encode_record_message_unchecked(make_record()), MUST(Messages::RustEchoServer::EchoValue::static_encode(RustEcho::Value { make_record() })).take_data());

    Vector<Function<void(RustEcho::Record&)>> invalidations;
    invalidations.append([](auto& record) { record.id = 0; });
    invalidations.append([](auto& record) { record.label = "123456789"_string; });
    invalidations.append([](auto& record) { record.name = "123456789"sv; });
    invalidations.append([](auto& record) { record.bytes = MUST(ByteBuffer::copy("123456789"sv.bytes())); });
    invalidations.append([](auto& record) { record.values.append(4); });
    invalidations.append([](auto& record) { record.scale = NAN; });
    invalidations.append([](auto& record) { record.scale = INFINITY; });
    for (auto& invalidate : invalidations) {
        auto record = make_record();
        invalidate(record);
        EXPECT(Messages::RustEchoServer::EchoValue::static_encode(RustEcho::Value { record }).is_error());
        expect_refused_by_cpp_and_rust(encode_record_message_unchecked(record));
    }

    // NB: The variant's index follows the endpoint magic and message ID, and a shape follows its index.
    auto message = MUST(Messages::RustEchoServer::EchoValue::static_encode(RustEcho::Value { RustEcho::Shape::Square })).take_data();
    message[9] = 2;
    expect_refused_by_cpp_and_rust(message);
    message[8] = 3;
    message[9] = 0;
    expect_refused_by_cpp_and_rust(message);
}

TEST_CASE(declared_enums_are_checked_wherever_they_are_encoded)
{
    // NB: This file encodes and decodes the enum itself, which must use its generated codec and not LibIPC's codec
    //     for any enum, which takes every value.
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    EXPECT(encoder.encode(static_cast<RustEcho::Shape>(2)).is_error());

    Array<u8, 1> invalid_shape { 2 };
    FixedMemoryStream stream { invalid_shape.span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    EXPECT(decoder.decode<RustEcho::Shape>().is_error());
}
