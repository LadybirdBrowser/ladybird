/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Platform.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Variant.h>

#if !defined(AK_OS_MACH)
#    error "TransportBootstrapMach is only available on Mach platforms"
#endif

#include <AK/Mutex.h>
#include <AK/RefPtr.h>
#include <LibCore/MachPort.h>
#include <LibThreading/Forward.h>

namespace IPC {

struct TransportBootstrapMachPorts {
    Core::MachPort receive_right;
    Core::MachPort send_right;
};

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_mach_server(StringView server_name);
ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_server_port(Core::MachPort const& server_port, AK::Duration reply_timeout = AK::Duration::from_seconds(5));

class TransportBootstrapMachServer {
    AK_MAKE_NONCOPYABLE(TransportBootstrapMachServer);

public:
    TransportBootstrapMachServer();
    ~TransportBootstrapMachServer();

    struct ChildTransportHandled {
    };
    struct OnDemandTransport {
        TransportBootstrapMachPorts ports;
    };
    using BootstrapRequestResult = Variant<ChildTransportHandled, OnDemandTransport>;

    // Hold this lock across process spawn and child transport registration so a
    // child bootstrap request cannot observe an unregistered pid.
    Mutex& child_registration_lock() { return m_child_registration_mutex; }

    // Must be called while holding child_registration_lock().
    // NB: Until the child asks for its ports, we hold them ourselves, so nothing would tell the other end of the
    //     transport that the child is gone. A child that exits before it asks is unregistered from a thread of our
    //     own, so that this also happens while the thread that spawned the child waits for the child to answer.
    void register_child_transport(pid_t, TransportBootstrapMachPorts);
    // For a child that was ended before it asked for its transport.
    void unregister_child_transport(pid_t);
    ErrorOr<BootstrapRequestResult> handle_bootstrap_request(pid_t, Core::MachPort reply_port);

private:
    static ErrorOr<void> send_transport_ports_to_child(Core::MachPort reply_port, TransportBootstrapMachPorts ports);
    static ErrorOr<TransportBootstrapMachPorts> create_on_demand_local_transport(Core::MachPort reply_port);

    struct ChildTransport {
        TransportBootstrapMachPorts ports;
        u64 serial { 0 };
    };

    void stop_watching_for_child_exit(pid_t);
    void child_exit_watcher_loop();

    Mutex m_child_registration_mutex;
    HashMap<pid_t, ChildTransport> m_child_transports;
    u64 m_next_child_transport_serial { 1 };

    int m_child_exit_queue { -1 };
    RefPtr<Threading::Thread> m_child_exit_watcher;
};

}
