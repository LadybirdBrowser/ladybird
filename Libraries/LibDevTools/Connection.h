/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibCore/Socket.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class Connection : public RefCounted<Connection> {
public:
    static NonnullRefPtr<Connection> create(NonnullOwnPtr<Core::BufferedTCPSocket>);
    ~Connection();

    Function<void()> on_connection_closed;
    Function<void(JsonObject const&)> on_message_received;

    void send_message(JsonValue const&);

private:
    explicit Connection(NonnullOwnPtr<Core::BufferedTCPSocket>);

    ErrorOr<void> on_ready_to_read();
    ErrorOr<JsonValue> read_message();

    NonnullOwnPtr<Core::BufferedTCPSocket> m_socket;
};

}
