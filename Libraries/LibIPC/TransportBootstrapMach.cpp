/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <LibCore/MachPort.h>
#include <LibIPC/RustFFI.h>
#include <LibIPC/TransportBootstrapMach.h>

namespace IPC {

static TransportBootstrapMachPorts adopt_transport_ports(u32 receive_right, u32 send_right)
{
    return TransportBootstrapMachPorts {
        .receive_right = Core::MachPort::adopt_right(receive_right, Core::MachPort::PortRight::Receive),
        .send_right = Core::MachPort::adopt_right(send_right, Core::MachPort::PortRight::Send),
    };
}

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_server_port(Core::MachPort const& server_port, AK::Duration reply_timeout)
{
    u32 receive_right = MACH_PORT_NULL;
    u32 send_right = MACH_PORT_NULL;
    i32 kernel_error = KERN_SUCCESS;
    auto const reply_timeout_in_milliseconds = static_cast<u32>(clamp<i64>(reply_timeout.to_milliseconds(), 0, NumericLimits<u32>::max()));
    switch (FFI::ipc_mach_bootstrap_transport_from_server_port(server_port.port(), reply_timeout_in_milliseconds, &receive_right, &send_right, &kernel_error)) {
    case FFI::BootstrapOutcome::Received:
        return adopt_transport_ports(receive_right, send_right);
    case FFI::BootstrapOutcome::KernelError:
        return Core::mach_error_to_error(kernel_error);
    case FFI::BootstrapOutcome::ServerClosedBeforeReplying:
        return Error::from_string_literal("Mach bootstrap server closed before replying");
    case FFI::BootstrapOutcome::MalformedReply:
        return Error::from_string_literal("Malformed Mach bootstrap reply");
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_mach_server(StringView server_name)
{
    auto server_port = TRY(Core::MachPort::look_up_from_bootstrap_server(ByteString { server_name }));
    return bootstrap_transport_from_server_port(server_port);
}

TransportBootstrapMachServer::TransportBootstrapMachServer()
    : m_server(FFI::ipc_mach_bootstrap_server_create())
{
}

TransportBootstrapMachServer::~TransportBootstrapMachServer()
{
    FFI::ipc_mach_bootstrap_server_destroy(m_server);
}

void TransportBootstrapMachServer::register_child_transport(pid_t pid, TransportBootstrapMachPorts ports)
{
    FFI::ipc_mach_bootstrap_server_register_child_transport(m_server, pid, ports.receive_right.release(), ports.send_right.release());
}

void TransportBootstrapMachServer::unregister_child_transport(pid_t pid)
{
    MutexLocker locker(m_child_registration_mutex);
    FFI::ipc_mach_bootstrap_server_unregister_child_transport(m_server, pid);
}

ErrorOr<TransportBootstrapMachServer::BootstrapRequestResult> TransportBootstrapMachServer::handle_bootstrap_request(pid_t pid, Core::MachPort reply_port)
{
    Optional<TransportBootstrapMachPorts> child_transport;
    {
        MutexLocker locker(m_child_registration_mutex);
        u32 receive_right = MACH_PORT_NULL;
        u32 send_right = MACH_PORT_NULL;
        if (FFI::ipc_mach_bootstrap_server_take_child_transport(m_server, pid, &receive_right, &send_right))
            child_transport = adopt_transport_ports(receive_right, send_right);
    }

    if (child_transport.has_value()) {
        auto ports = child_transport.release_value();
        auto const result = FFI::ipc_mach_bootstrap_send_transport_ports_to_child(reply_port.release(), ports.receive_right.release(), ports.send_right.release());
        if (result != KERN_SUCCESS)
            return Core::mach_error_to_error(result);
        return ChildTransportHandled {};
    }

    u32 receive_right = MACH_PORT_NULL;
    u32 send_right = MACH_PORT_NULL;
    auto const result = FFI::ipc_mach_bootstrap_create_on_demand_local_transport(reply_port.release(), &receive_right, &send_right);
    if (result != KERN_SUCCESS)
        return Core::mach_error_to_error(result);
    return OnDemandTransport { .ports = adopt_transport_ports(receive_right, send_right) };
}

}
