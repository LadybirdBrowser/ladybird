/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibCore/EventLoop.h>
#include <LibCore/MachPort.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibIPC/MachBootstrapMessages.h>
#include <LibIPC/TransportBootstrapMach.h>
#include <LibTest/TestCase.h>
#include <WebDriver/Client.h>
#include <WebDriver/Session.h>

TEST_CASE(queued_bootstrap_callbacks_are_discarded_when_browser_launch_fails)
{
    Core::EventLoop event_loop;
    auto server = TRY_OR_FAIL(Core::TCPServer::try_create());
    TRY_OR_FAIL(server->listen(IPv4Address(127, 0, 0, 1), 0));
    TRY_OR_FAIL(server->set_blocking(true));
    auto peer = TRY_OR_FAIL(Core::TCPSocket::connect("127.0.0.1", server->local_port().value()));
    auto socket = TRY_OR_FAIL(server->accept());
    auto buffered_socket = TRY_OR_FAIL(Core::BufferedTCPSocket::create(move(socket)));
    auto client = TRY_OR_FAIL(WebDriver::Client::try_create(move(buffered_socket), [](auto const& endpoint, bool) -> ErrorOr<Core::Process> {
        auto server_port = TRY(Core::MachPort::look_up_from_bootstrap_server(endpoint));
        for (size_t i = 0; i < 2; ++i) {
            IPC::MessageWithSelfTaskPort message {};
            message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
            message.header.msgh_size = sizeof(message);
            message.header.msgh_remote_port = server_port.port();
            message.header.msgh_id = IPC::SELF_TASK_PORT_MESSAGE_ID;
            message.body.msgh_descriptor_count = 1;
            message.port_descriptor.name = mach_task_self();
            message.port_descriptor.disposition = MACH_MSG_TYPE_COPY_SEND;
            message.port_descriptor.type = MACH_MSG_PORT_DESCRIPTOR;
            VERIFY(mach_msg_send(&message.header) == KERN_SUCCESS);
        }

        // NB: The successful reply is a barrier for the earlier requests. No event-loop work has run yet.
        auto ports = TRY(IPC::bootstrap_transport_from_mach_server(endpoint));
        return Error::from_string_literal("Browser launch failed after queuing bootstrap callbacks");
    }));

    auto result = WebDriver::Session::create(client, JsonObject {}, Web::WebDriver::SessionFlags::Http);
    EXPECT(result.is_error());
    EXPECT(!WebDriver::Session::has_pending_http_session_creation());
    EXPECT_EQ(WebDriver::Session::session_count(Web::WebDriver::SessionFlags::Default), 0u);

    // NB: Closing the listener joins the callback thread, so every queued callback is ready to dispatch.
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
}
