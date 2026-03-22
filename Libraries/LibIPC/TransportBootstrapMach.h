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

    struct WaitingForChildTransport {
    };
    struct OnDemandTransport {
        TransportBootstrapMachPorts ports;
    };
    using RegisterReplyPortResult = Variant<WaitingForChildTransport, OnDemandTransport>;

    void register_pending_transport(pid_t, TransportBootstrapMachPorts);
    ErrorOr<RegisterReplyPortResult> register_reply_port(pid_t, Core::MachPort reply_port);

private:
    struct WaitingForPorts {
        TransportBootstrapMachPorts ports;
    };
    struct WaitingForReplyPort {
        Core::MachPort reply_port;
    };
    using PendingBootstrapState = Variant<WaitingForPorts, WaitingForReplyPort>;

    static void send_transport_ports_to_child(Core::MachPort reply_port, TransportBootstrapMachPorts ports);
    static ErrorOr<TransportBootstrapMachPorts> create_on_demand_local_transport(Core::MachPort reply_port);

    Threading::Mutex m_pending_bootstrap_mutex;
    HashMap<pid_t, PendingBootstrapState> m_pending_bootstrap;
};

}
