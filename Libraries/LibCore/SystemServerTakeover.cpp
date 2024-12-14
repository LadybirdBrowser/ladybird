/*
 * Copyright (c) 2022, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SystemServerTakeover.h"
#include <LibCore/Environment.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>

namespace Core {

HashMap<ByteString, int> s_overtaken_sockets {};
bool s_overtaken_sockets_parsed { false };

static void parse_sockets_from_system_server()
{
    VERIFY(!s_overtaken_sockets_parsed);

    constexpr auto socket_takeover = "SOCKET_TAKEOVER"sv;
    auto sockets = Environment::get(socket_takeover);
    if (!sockets.has_value()) {
        s_overtaken_sockets_parsed = true;
        return;
    }

    for (auto socket : sockets->split_view(';')) {
        auto params = socket.split_view(':');
        VERIFY(params.size() == 2);
        s_overtaken_sockets.set(params[0].to_byte_string(), params[1].to_number<int>().value());
    }

    s_overtaken_sockets_parsed = true;
    // We wouldn't want our children to think we're passing
    // them a socket either, so unset the env variable.
    MUST(Environment::unset(socket_takeover));
}

ErrorOr<NonnullOwnPtr<Core::LocalSocket>> take_over_socket_from_system_server(ByteString const& socket_path)
{
    if (!s_overtaken_sockets_parsed)
        parse_sockets_from_system_server();

    int fd;
    if (socket_path.is_empty()) {
        // We want the first (and only) socket.
        VERIFY(s_overtaken_sockets.size() == 1);
        fd = s_overtaken_sockets.begin()->value;
    } else {
        auto it = s_overtaken_sockets.find(socket_path);
        if (it == s_overtaken_sockets.end())
            return Error::from_string_literal("Non-existent socket requested");
        fd = it->value;
    }

    // Sanity check: it has to be a socket.
    if (!System::is_socket(fd))
        return Error::from_string_literal("The fd or handle we got from SystemServer is not a socket");

    auto socket = TRY(Core::LocalSocket::adopt_fd(fd));
    // It had to be !CLOEXEC for obvious reasons, but we
    // don't need it to be !CLOEXEC anymore, so set the
    // CLOEXEC flag now.
    TRY(socket->set_close_on_exec(true));

    return socket;
}

}
