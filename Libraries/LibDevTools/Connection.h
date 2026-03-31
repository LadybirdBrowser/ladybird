/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <LibCore/Socket.h>
#include <LibDevTools/Export.h>

namespace DevTools {

class DEVTOOLS_API Connection : public RefCounted<Connection> {
public:
    static NonnullRefPtr<Connection> create(NonnullOwnPtr<Core::BufferedTCPSocket>);
    virtual ~Connection();

    Function<void()> on_connection_closed;
    Function<void(JsonObject)> on_message_received;

    virtual void send_message(JsonValue const&);

protected:
    Connection() = default;
    explicit Connection(NonnullOwnPtr<Core::BufferedTCPSocket>);

private:
    ErrorOr<void> on_ready_to_read();
    ErrorOr<JsonValue> read_message();

    OwnPtr<Core::BufferedTCPSocket> m_socket;
};

}
