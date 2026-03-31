/*
 * Copyright (c) 2025, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Connection.h>

namespace DevTools {

class DEVTOOLS_API CallbackTransport : public Connection {
public:
    using SendHandler = Function<void(JsonValue const&)>;

    static NonnullRefPtr<CallbackTransport> create(SendHandler handler)
    {
        return adopt_ref(*new CallbackTransport(move(handler)));
    }

    void send_message(JsonValue const& message) override
    {
        if (m_send_handler)
            m_send_handler(message);
    }

    void set_send_handler(SendHandler handler)
    {
        m_send_handler = move(handler);
    }

    void receive_message(JsonObject message)
    {
        if (on_message_received)
            on_message_received(move(message));
    }

private:
    explicit CallbackTransport(SendHandler handler)
        : m_send_handler(move(handler))
    {
    }

    SendHandler m_send_handler;
};

}
