/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibWeb/WebAudio/Worklet/MessagePortTransport.h>

namespace Web::WebAudio::Render {

static constexpr u8 message_port_ipc_file_tag = 0xA5;

ErrorOr<void> attach_message_port_transport_from_fd(HTML::MessagePort& port, int fd)
{
    if (fd < 0)
        return Error::from_string_literal("Invalid MessagePort transport fd");

    HTML::TransferDataEncoder encoder;
    encoder.encode(message_port_ipc_file_tag);
    encoder.encode(IPC::File::adopt_fd(fd));

    HTML::TransferDataDecoder decoder { move(encoder) };
    MUST(port.transfer_receiving_steps(decoder));
    return {};
}

ErrorOr<MessagePortTransportPair> create_message_port_transport_pair(JS::Realm& realm)
{
    int fds[2] = { -1, -1 };
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    auto socket0 = TRY(Core::LocalSocket::adopt_fd(fds[0]));
    auto socket1 = TRY(Core::LocalSocket::adopt_fd(fds[1]));

    TRY(socket0->set_blocking(false));
    TRY(socket0->set_close_on_exec(true));
    TRY(socket1->set_blocking(false));
    TRY(socket1->set_close_on_exec(true));

    int fd0 = TRY(socket0->release_fd());
    int fd1 = TRY(socket1->release_fd());

    GC::Ref<HTML::MessagePort> port = HTML::MessagePort::create(realm);
    TRY(attach_message_port_transport_from_fd(port, fd0));

    return MessagePortTransportPair {
        .port = port,
        .peer_fd = fd1,
    };
}

}
