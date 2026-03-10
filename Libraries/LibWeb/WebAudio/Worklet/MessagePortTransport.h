/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::WebAudio::Render {

struct MessagePortTransportPair {
    GC::Ref<HTML::MessagePort> port;
    int peer_fd { -1 };
};

ErrorOr<void> attach_message_port_transport_from_fd(HTML::MessagePort&, int fd);
ErrorOr<MessagePortTransportPair> create_message_port_transport_pair(JS::Realm&);

}
