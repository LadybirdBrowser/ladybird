/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibIPC/MachBootstrapListener.h>
#include <LibIPC/RustFFI.h>

namespace IPC {

MachBootstrapListener::MachBootstrapListener(ByteString server_port_name)
    : m_server_port_name(move(server_port_name))
{
    auto handle_request = [](void* listener, i32 pid, u32 task_name_port, u32 reply_port) {
        auto& self = *static_cast<MachBootstrapListener*>(listener);
        VERIFY(self.on_bootstrap_request);
        self.on_bootstrap_request({
            .pid = pid,
            .task_name_port = Core::MachPort::adopt_right(task_name_port, Core::MachPort::PortRight::Send),
            .reply_port = Core::MachPort::adopt_right(reply_port, Core::MachPort::PortRight::SendOnce),
        });
    };
    m_listener = FFI::ipc_mach_bootstrap_listener_create(m_server_port_name.bytes(), this, handle_request);
    if (!m_listener)
        dbgln("Failed to allocate server port");
}

MachBootstrapListener::~MachBootstrapListener()
{
    if (!m_listener)
        return;
    // NB: Destroying the listener stops it, after which nothing reaches this object.
    FFI::ipc_mach_bootstrap_listener_destroy(m_listener);
}

void MachBootstrapListener::stop()
{
    if (m_listener)
        FFI::ipc_mach_bootstrap_listener_stop(m_listener);
}

bool MachBootstrapListener::is_initialized()
{
    return m_listener;
}

}
