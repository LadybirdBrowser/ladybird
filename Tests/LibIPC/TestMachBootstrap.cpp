/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Mutex.h>
#include <AK/Random.h>
#include <LibCore/MachPort.h>
#include <LibIPC/MachBootstrapListener.h>
#include <LibIPC/TransportBootstrapMach.h>
#include <LibTest/TestCase.h>
#include <unistd.h>

static ByteString server_name()
{
    return ByteString::formatted("org.ladybird.TestMachBootstrap.{}", generate_random_uuid());
}

TEST_CASE(idle_listener_can_be_stopped_repeatedly)
{
    IPC::MachBootstrapListener listener { server_name() };
    EXPECT(listener.is_initialized());
    listener.stop();
    listener.stop();
}

TEST_CASE(bootstrap_round_trip)
{
    IPC::TransportBootstrapMachServer server;
    Optional<IPC::TransportBootstrapMachPorts> server_ports;
    IPC::MachBootstrapListener listener { server_name() };
    EXPECT(listener.is_initialized());
    listener.on_bootstrap_request = [&](auto request) {
        auto result = MUST(server.handle_bootstrap_request(request.pid, move(request.reply_port)));
        VERIFY(result.template has<IPC::TransportBootstrapMachServer::OnDemandTransport>());
        server_ports = move(result.template get<IPC::TransportBootstrapMachServer::OnDemandTransport>().ports);
    };

    auto ports = TRY_OR_FAIL(IPC::bootstrap_transport_from_mach_server(listener.server_port_name()));
    EXPECT(MACH_PORT_VALID(ports.receive_right.port()));
    EXPECT(MACH_PORT_VALID(ports.send_right.port()));
    listener.stop();
    EXPECT(server_ports.has_value());
}

TEST_CASE(bootstrap_reports_a_server_that_drops_the_reply)
{
    IPC::MachBootstrapListener listener { server_name() };
    EXPECT(listener.is_initialized());
    listener.on_bootstrap_request = [](auto) { };

    EXPECT(IPC::bootstrap_transport_from_mach_server(listener.server_port_name()).is_error());
}

TEST_CASE(bootstrap_reply_to_a_closed_peer_releases_the_transport_ports)
{
    auto reply_receive_right = TRY_OR_FAIL(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    mach_port_t reply_port = MACH_PORT_NULL;
    mach_msg_type_name_t right_type = 0;
    VERIFY(mach_port_extract_right(mach_task_self(), reply_receive_right.port(), MACH_MSG_TYPE_MAKE_SEND_ONCE, &reply_port, &right_type) == KERN_SUCCESS);
    auto reply_send_once_right = Core::MachPort::adopt_right(reply_port, Core::MachPort::PortRight::SendOnce);
    reply_receive_right = {};

    auto receive_right = TRY_OR_FAIL(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto peer_receive_right = TRY_OR_FAIL(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto send_right = TRY_OR_FAIL(peer_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));
    auto receive_port = receive_right.port();
    auto send_port = send_right.port();

    IPC::TransportBootstrapMachServer server;
    {
        MutexLocker locker(server.child_registration_lock());
        server.register_child_transport(getpid(), { move(receive_right), move(send_right) });
    }
    // NB: Mach can silently consume a send-once message whose destination died. Either outcome must release its rights.
    (void)server.handle_bootstrap_request(getpid(), move(reply_send_once_right));

    mach_port_type_t type = 0;
    EXPECT_EQ(mach_port_type(mach_task_self(), reply_port, &type), KERN_INVALID_NAME);
    EXPECT_EQ(mach_port_type(mach_task_self(), receive_port, &type), KERN_INVALID_NAME);
    EXPECT_EQ(mach_port_type(mach_task_self(), send_port, &type), KERN_SUCCESS);
    EXPECT_EQ(type, static_cast<mach_port_type_t>(MACH_PORT_TYPE_RECEIVE));
}

TEST_CASE(bootstrap_reports_a_missing_reply_port)
{
    auto receive_right = TRY_OR_FAIL(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto peer_receive_right = TRY_OR_FAIL(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto send_right = TRY_OR_FAIL(peer_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));
    auto receive_port = receive_right.port();
    auto send_port = send_right.port();

    IPC::TransportBootstrapMachServer server;
    {
        MutexLocker locker(server.child_registration_lock());
        server.register_child_transport(getpid(), { move(receive_right), move(send_right) });
    }
    EXPECT(server.handle_bootstrap_request(getpid(), {}).is_error());

    mach_port_type_t type = 0;
    EXPECT_EQ(mach_port_type(mach_task_self(), receive_port, &type), KERN_INVALID_NAME);
    EXPECT_EQ(mach_port_type(mach_task_self(), send_port, &type), KERN_SUCCESS);
    EXPECT_EQ(type, static_cast<mach_port_type_t>(MACH_PORT_TYPE_RECEIVE));
}
