/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::WebAudio::Render {

struct MessagePortTransportPair {
    GC::Ref<HTML::MessagePort> port;
    IPC::TransportHandle peer_handle;
};

ErrorOr<void> attach_message_port_transport(HTML::MessagePort&, IPC::TransportHandle);
ErrorOr<MessagePortTransportPair> create_message_port_transport_pair(JS::Realm&);

} // namespace Web::WebAudio::Render
