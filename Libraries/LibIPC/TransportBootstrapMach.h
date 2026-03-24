/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Platform.h>
#include <AK/StringView.h>
#include <AK/Variant.h>

#if !defined(AK_OS_MACH)
#    error "TransportBootstrapMach is only available on Mach platforms"
#endif

#include <LibCore/MachPort.h>
#include <LibThreading/Mutex.h>

namespace IPC {

struct TransportBootstrapMachPorts {
    Core::MachPort receive_right;
    Core::MachPort send_right;
};

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_mach_server(StringView server_name);
ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_server_port(Core::MachPort const& server_port);

class TransportBootstrapMachServer {
    AK_MAKE_NONCOPYABLE(TransportBootstrapMachServer);

public:
    TransportBootstrapMachServer() = default;

    struct ChildTransportHandled {
    };
    struct OnDemandTransport {
        TransportBootstrapMachPorts ports;
    };
    using BootstrapRequestResult = Variant<ChildTransportHandled, OnDemandTransport>;

    // Hold this lock across process spawn and child transport registration so a
    // child bootstrap request cannot observe an unregistered pid.
    Threading::Mutex& child_registration_lock() { return m_child_registration_mutex; }

    // Must be called while holding child_registration_lock().
    void register_child_transport(pid_t, TransportBootstrapMachPorts);
    ErrorOr<BootstrapRequestResult> handle_bootstrap_request(pid_t, Core::MachPort reply_port);

private:
    static void send_transport_ports_to_child(Core::MachPort reply_port, TransportBootstrapMachPorts ports);
    static ErrorOr<TransportBootstrapMachPorts> create_on_demand_local_transport(Core::MachPort reply_port);

    Threading::Mutex m_child_registration_mutex;
    HashMap<pid_t, TransportBootstrapMachPorts> m_child_transports;
};

}
