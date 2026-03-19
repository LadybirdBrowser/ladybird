/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/Transport.h>

#if defined(AK_OS_MACOS)
#    include <LibIPC/TransportBootstrapMach.h>
#else
#    include <LibCore/SystemServerTakeover.h>
#endif

namespace IPC {

template<typename ConnectionFromClientType, typename... Args>
ErrorOr<NonnullRefPtr<ConnectionFromClientType>> take_over_accepted_client_from_system_server([[maybe_unused]] StringView mach_server_name, Args&&... args)
{
#if defined(AK_OS_MACOS)
    auto ports = TRY(bootstrap_transport_from_mach_server(mach_server_name));
    return IPC::new_client_connection<ConnectionFromClientType>(
        make<IPC::Transport>(move(ports.receive_right), move(ports.send_right)),
        forward<Args>(args)...);
#else
    auto socket = TRY(Core::take_over_socket_from_system_server());
    return IPC::new_client_connection<ConnectionFromClientType>(make<IPC::Transport>(move(socket)), forward<Args>(args)...);
#endif
}

}
