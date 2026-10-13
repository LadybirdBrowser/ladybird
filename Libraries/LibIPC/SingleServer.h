/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/StartupTransport.h>
#include <LibIPC/Transport.h>

namespace IPC {

template<typename ConnectionFromClientType, typename... Args>
ErrorOr<NonnullRefPtr<ConnectionFromClientType>> take_over_accepted_client_from_system_server(StringView mach_server_name, Args&&... args)
{
    auto transport = TRY(take_over_startup_transport(mach_server_name));
    return IPC::new_client_connection<ConnectionFromClientType>(move(transport), forward<Args>(args)...);
}

}
