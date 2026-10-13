/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/StartupTransport.h>
#include <LibIPC/Transport.h>

#if defined(AK_OS_MACOS)
#    include <LibIPC/TransportBootstrapMach.h>
#else
#    include <LibCore/SystemServerTakeover.h>
#endif

#if defined(AK_OS_WINDOWS)
#    include <LibIPC/HandleBroker.h>
#endif

namespace IPC {

#if defined(AK_OS_MACOS)

ErrorOr<NonnullOwnPtr<Transport>> take_over_startup_transport(StringView mach_server_name)
{
    auto ports = TRY(bootstrap_transport_from_mach_server(mach_server_name));
    return make<Transport>(move(ports.receive_right), move(ports.send_right));
}

#else

ErrorOr<NonnullOwnPtr<Transport>> take_over_startup_transport(StringView)
{
#    if defined(AK_OS_WINDOWS)
    auto broker_socket = TRY(Core::take_over_socket_from_system_server("handle-broker"));
    broker_socket->set_notifications_enabled(false);
    TRY(broker_socket->set_blocking(false));
    HandleBroker::install(make<HandleBrokerClient>(make<Transport>(move(broker_socket))));
#    endif

    auto socket = TRY(Core::take_over_socket_from_system_server("ipc"));
    return make<Transport>(move(socket));
}

#endif

}
