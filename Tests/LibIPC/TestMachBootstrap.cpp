/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Mutex.h>
#include <AK/Random.h>
#include <AK/Time.h>
#include <LibCore/MachPort.h>
#include <LibIPC/MachBootstrapListener.h>
#include <LibIPC/MachBootstrapMessages.h>
#include <LibIPC/TransportBootstrapMach.h>
#include <LibTest/TestCase.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <spawn.h>
#include <sys/wait.h>
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

TEST_CASE(bootstrap_sends_only_the_task_name_port)
{
    IPC::TransportBootstrapMachServer server;
    Optional<Core::MachPort> received_task_name_port;
    IPC::MachBootstrapListener listener { server_name() };
    EXPECT(listener.is_initialized());
    listener.on_bootstrap_request = [&](auto request) {
        received_task_name_port = move(request.task_name_port);
        (void)server.handle_bootstrap_request(request.pid, move(request.reply_port));
    };

    (void)TRY_OR_FAIL(IPC::bootstrap_transport_from_mach_server(listener.server_port_name()));
    listener.stop();
    VERIFY(received_task_name_port.has_value());
    auto port = received_task_name_port->port();
    EXPECT_NE(port, mach_task_self());

    // The name port is enough for process statistics.
    mach_task_basic_info_data_t basic_info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    EXPECT_EQ(task_info(port, MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&basic_info), &count), KERN_SUCCESS);

    // It must not grant control over the sender, such as access to its memory.
    mach_vm_address_t address = 0;
    EXPECT_NE(mach_vm_allocate(port, &address, PAGE_SIZE, VM_FLAGS_ANYWHERE), KERN_SUCCESS);
}

static void send_raw_message(Core::MachPort const& server_port, mach_msg_header_t& header)
{
    header.msgh_remote_port = server_port.port();
    VERIFY(mach_msg(&header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, header.msgh_size, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL) == KERN_SUCCESS);
}

TEST_CASE(listener_ignores_malformed_messages)
{
    IPC::TransportBootstrapMachServer server;
    size_t request_count = 0;
    IPC::MachBootstrapListener listener { server_name() };
    EXPECT(listener.is_initialized());
    listener.on_bootstrap_request = [&](auto request) {
        ++request_count;
        (void)server.handle_bootstrap_request(request.pid, move(request.reply_port));
    };
    auto server_port = TRY_OR_FAIL(Core::MachPort::look_up_from_bootstrap_server(listener.server_port_name()));

    // A message with an unknown id.
    {
        mach_msg_header_t header {};
        header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
        header.msgh_size = sizeof(header);
        header.msgh_id = 0x1234;
        send_raw_message(server_port, header);
    }

    // The bootstrap id without the task name port.
    {
        mach_msg_header_t header {};
        header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
        header.msgh_size = sizeof(header);
        header.msgh_id = IPC::SELF_TASK_NAME_PORT_MESSAGE_ID;
        send_raw_message(server_port, header);
    }

    // A receive right where the task name port belongs.
    {
        auto receive_right = TRY_OR_FAIL(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
        IPC::MessageWithSelfTaskNamePort message {};
        message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
        message.header.msgh_size = sizeof(message);
        message.header.msgh_id = IPC::SELF_TASK_NAME_PORT_MESSAGE_ID;
        message.body.msgh_descriptor_count = 1;
        message.port_descriptor.name = receive_right.release();
        message.port_descriptor.disposition = MACH_MSG_TYPE_MOVE_RECEIVE;
        message.port_descriptor.type = MACH_MSG_PORT_DESCRIPTOR;
        send_raw_message(server_port, message.header);
    }

    // A message that is too large for the receive buffer.
    {
        struct {
            mach_msg_header_t header;
            u8 data[4096];
        } message {};
        message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
        message.header.msgh_size = sizeof(message);
        message.header.msgh_id = IPC::SELF_TASK_NAME_PORT_MESSAGE_ID;
        send_raw_message(server_port, message.header);
    }

    // The listener must still serve a well-formed request after all of the above.
    auto ports = TRY_OR_FAIL(IPC::bootstrap_transport_from_mach_server(listener.server_port_name()));
    EXPECT(MACH_PORT_VALID(ports.receive_right.port()));
    listener.stop();
    EXPECT_EQ(request_count, 1u);
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

enum class RegisterChild {
    WhileItRuns,
    AfterItExited,
};

// Blocks until the server lets go of the ports of a child that exits before it asks for them.
static void expect_ports_of_exited_child_to_be_released(RegisterChild register_child)
{
    auto local_receive_right = TRY_OR_FAIL(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto child_send_right = TRY_OR_FAIL(local_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));
    auto child_receive_right = TRY_OR_FAIL(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto child_receive_port = child_receive_right.port();

    // NB: This is how a transport learns that its peer is gone.
    auto notification_receive_right = TRY_OR_FAIL(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    mach_port_t previous = MACH_PORT_NULL;
    VERIFY(mach_port_request_notification(mach_task_self(), local_receive_right.port(), MACH_NOTIFY_NO_SENDERS, 1, notification_receive_right.port(), MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous) == KERN_SUCCESS);

    IPC::TransportBootstrapMachServer server;
    pid_t pid = -1;
    {
        MutexLocker locker(server.child_registration_lock());
        // NB: The child runs until its input ends, which is when we close our end of the pipe.
        int input[2] = {};
        VERIFY(pipe(input) == 0);
        posix_spawn_file_actions_t file_actions;
        VERIFY(posix_spawn_file_actions_init(&file_actions) == 0);
        VERIFY(posix_spawn_file_actions_adddup2(&file_actions, input[0], STDIN_FILENO) == 0);
        VERIFY(posix_spawn_file_actions_addclose(&file_actions, input[1]) == 0);
        char const* arguments[] = { "/bin/cat", nullptr };
        VERIFY(posix_spawn(&pid, arguments[0], &file_actions, nullptr, const_cast<char**>(arguments), nullptr) == 0);
        VERIFY(posix_spawn_file_actions_destroy(&file_actions) == 0);
        VERIFY(close(input[0]) == 0);
        if (register_child == RegisterChild::AfterItExited) {
            VERIFY(close(input[1]) == 0);
            siginfo_t info {};
            while (waitid(P_PID, pid, &info, WEXITED | WNOWAIT) < 0)
                VERIFY(errno == EINTR);
        }
        server.register_child_transport(pid, { move(child_receive_right), move(child_send_right) });
        if (register_child == RegisterChild::WhileItRuns)
            VERIFY(close(input[1]) == 0);
    }

    struct {
        mach_msg_header_t header;
        u8 body[64];
    } notification {};
    // NB: The limit only keeps a server that holds on to the ports from stopping the test run.
    static constexpr mach_msg_timeout_t limit_in_milliseconds = 60'000;
    EXPECT_EQ(mach_msg(&notification.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(notification), notification_receive_right.port(), limit_in_milliseconds, MACH_PORT_NULL), KERN_SUCCESS);
    EXPECT_EQ(notification.header.msgh_id, MACH_NOTIFY_NO_SENDERS);

    // The server lets go of the send right first, so the receive right may still be there for a moment.
    mach_port_type_t type = 0;
    auto deadline = MonotonicTime::now() + AK::Duration::from_milliseconds(limit_in_milliseconds);
    while (mach_port_type(mach_task_self(), child_receive_port, &type) != KERN_INVALID_NAME && MonotonicTime::now() < deadline)
        usleep(1'000);
    EXPECT_EQ(mach_port_type(mach_task_self(), child_receive_port, &type), KERN_INVALID_NAME);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        VERIFY(errno == EINTR);
}

TEST_CASE(ports_of_a_child_that_exits_before_taking_them_are_released)
{
    expect_ports_of_exited_child_to_be_released(RegisterChild::WhileItRuns);
}

TEST_CASE(ports_of_a_child_that_exited_before_registration_are_released)
{
    expect_ports_of_exited_child_to_be_released(RegisterChild::AfterItExited);
}
