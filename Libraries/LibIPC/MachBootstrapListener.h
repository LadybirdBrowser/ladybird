/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Platform.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibCore/MachPort.h>

#if !defined(AK_OS_MACH)
#    error "MachBootstrapListener is only available on Mach kernel-based OS's"
#endif

namespace IPC {

namespace FFI {

struct BootstrapListener;

}

class MachBootstrapListener {
    AK_MAKE_NONCOPYABLE(MachBootstrapListener);

public:
    AK_ALLOC_WITH_KMALLOC;

    // Registers a port under the name with the bootstrap server, and takes requests there on a thread of its own.
    explicit MachBootstrapListener(ByteString server_port_name);
    ~MachBootstrapListener();

    void stop();

    bool is_initialized();

    struct BootstrapRequest {
        pid_t pid { -1 };
        Core::MachPort task_name_port;
        Core::MachPort reply_port;
    };
    Function<void(BootstrapRequest)> on_bootstrap_request;

    ByteString const& server_port_name() const { return m_server_port_name; }

private:
    ByteString const m_server_port_name;
    FFI::BootstrapListener* m_listener { nullptr };
};

}
