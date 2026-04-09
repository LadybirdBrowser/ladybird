/*
 * Copyright (c) 2022, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Export.h>
#include <LibCore/Socket.h>

namespace Core {

CORE_API ErrorOr<NonnullOwnPtr<Core::LocalSocket>> take_over_socket_from_system_server(ByteString const& socket_path = {});

}
