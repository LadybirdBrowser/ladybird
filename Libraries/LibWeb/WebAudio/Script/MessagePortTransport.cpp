/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Transport.h>
#include <LibWeb/WebAudio/Script/MessagePortTransport.h>

namespace Web::WebAudio::Render {

static constexpr u8 message_port_ipc_file_tag = 0xA5;

ErrorOr<void> attach_message_port_transport(HTML::MessagePort& port, IPC::TransportHandle handle)
{
    HTML::TransferDataEncoder encoder;
    encoder.encode(Vector<HTML::SerializedTransferRecord> {});
    encoder.encode(Vector<HTML::SerializedTransferRecord> {});
    encoder.encode(false);
    encoder.encode(message_port_ipc_file_tag);
    encoder.encode(handle);

    HTML::TransferDataDecoder decoder { move(encoder) };
    MUST(port.transfer_receiving_steps(decoder));
    return {};
}

ErrorOr<MessagePortTransportPair> create_message_port_transport_pair(JS::Realm& realm)
{
    auto paired = TRY(IPC::Transport::create_paired());
    auto local_handle = TRY(paired.local->release_for_transfer());

    GC::Ref<HTML::MessagePort> port = HTML::MessagePort::create(realm);
    TRY(attach_message_port_transport(port, move(local_handle)));

    return MessagePortTransportPair {
        .port = port,
        .peer_handle = move(paired.remote_handle),
    };
}

} // namespace Web::WebAudio::Render
