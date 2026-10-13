/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StringView.h>
#include <LibIPC/Forward.h>

namespace IPC {

// Takes over the connection to the Browser that this process was spawned with. On Windows, this also takes over the
// line to the Browser's handle broker, which becomes this process's HandleBroker.
ErrorOr<NonnullOwnPtr<Transport>> take_over_startup_transport(StringView mach_server_name);

}
