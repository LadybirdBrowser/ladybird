/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibIPC/TransportSocket.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    if (size < 2 || size > 4096)
        return 0;
    Fuzzing::BoundedInput input({ data, size });
    auto mode = input.byte();
    auto fragment_size = 1 + input.byte() % 64;
    int fds[2] {};
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));
    auto socket = MUST(Core::LocalSocket::adopt_fd(fds[0]));
    auto peer = MUST(Core::LocalSocket::adopt_fd(fds[1]));
    MUST(socket->set_blocking(false));
    MUST(peer->set_blocking(false));
    IPC::TransportSocket transport(move(socket));
    size_t delivered = 0;
    auto send_fragments = [&](ReadonlyBytes bytes, Optional<int> attachment) {
        while (!bytes.is_empty()) {
            auto count = min(bytes.size(), static_cast<size_t>(fragment_size));
            Vector<int, 1> rights;
            if (attachment.has_value())
                rights.append(attachment.value());
            auto sent = peer->send_message(bytes.slice(0, count), MSG_NOSIGNAL, move(rights));
            if (sent.is_error() || sent.value() == 0)
                return false;
            bytes = bytes.slice(sent.value());
            attachment.clear();
            transport.wait_until_incoming_is_current();
        }
        return true;
    };
    if (!(mode & 1)) {
        for (size_t i = 0; i < 8 && !input.remaining().is_empty(); ++i) {
            auto with_fd = input.byte() & 1;
            auto payload = input.take(1 + input.byte() % 64);
            // Empty payloads have separate wire semantics; use at least one byte.
            if (payload.is_empty())
                break;
            int pipe_fds[2] {};
            if (::pipe2(pipe_fds, O_CLOEXEC) != 0)
                return 0;
            auto read_end = IPC::File::adopt_fd(pipe_fds[0]);
            auto write_end = IPC::File::adopt_fd(pipe_fds[1]);
            u8 token = static_cast<u8>(i);
            MUST(Core::System::write(write_end.fd(), ReadonlyBytes { &token, 1 }));
            IPC::SocketMessageHeader header {
                .type = IPC::SocketMessageHeader::Type::Payload,
                .payload_size = static_cast<u32>(payload.size()),
                .fd_count = static_cast<u32>(with_fd),
            };
            auto header_bytes = ReadonlyBytes { reinterpret_cast<u8 const*>(&header), sizeof(header) };
            if (!send_fragments(header_bytes, with_fd ? Optional<int> { read_end.fd() } : Optional<int> {}))
                return 0;
            if (!send_fragments(payload, {}))
                return 0;
            auto before = delivered;
            (void)transport.read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
                ++delivered;
                VERIFY(message.bytes.bytes() == payload);
                VERIFY(message.attachments.size() == static_cast<size_t>(with_fd));
                if (with_fd) {
                    auto received_fd = IPC::File::adopt_fd(message.attachments.dequeue().to_fd());
                    VERIFY(::fcntl(received_fd.fd(), F_GETFD) >= 0);
                    // Do not block if a bug delivered the wrong valid descriptor.
                    auto flags = ::fcntl(received_fd.fd(), F_GETFL);
                    VERIFY(flags >= 0 && ::fcntl(received_fd.fd(), F_SETFL, flags | O_NONBLOCK) == 0);
                    u8 received_token = 0;
                    auto read = MUST(Core::System::read(received_fd.fd(), Bytes { &received_token, 1 }));
                    VERIFY(read == 1 && received_token == token);
                }
            });
            VERIFY(delivered == before + 1);
        }
    } else {
        // Raw malformed framing/acknowledgements: disconnect is expected, not failure.
        (void)send_fragments(input.remaining(), {});
    }
    // EOF after complete or partial frames; the transport destructor joins its I/O thread.
    peer->close();
    transport.wait_until_incoming_is_current();
    (void)transport.read_as_many_messages_as_possible_without_blocking([](auto&&) { });
    return 0;
}
