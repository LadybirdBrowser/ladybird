/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/HandleBrokerMessages.h>
#include <LibIPC/HandleBrokerServer.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>
#include <LibThreading/Thread.h>

#include <AK/Windows.h>

namespace IPC {

class HandleBrokerChildConnection {
    AK_MAKE_NONCOPYABLE(HandleBrokerChildConnection);
    AK_MAKE_NONMOVABLE(HandleBrokerChildConnection);

public:
    AK_ALLOC_WITH_KMALLOC;

    HandleBrokerChildConnection(HandleBrokerServer& server, pid_t pid, NonnullOwnPtr<Transport> transport, HANDLE process_handle)
        : m_server(server)
        , m_pid(pid)
        , m_transport(move(transport))
        , m_process_handle(process_handle)
    {
    }

    ~HandleBrokerChildConnection()
    {
        CloseHandle(m_process_handle);
    }

    void start()
    {
        m_transport->set_up_read_hook([this] {
            serve_requests();
        });
    }

    pid_t pid() const { return m_pid; }

private:
    void serve_requests()
    {
        if (m_is_closing)
            return;

        bool misbehaved = false;
        auto should_shutdown = m_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            if (misbehaved)
                return;
            FixedMemoryStream stream { message.bytes.bytes() };
            Decoder decoder { stream, message.attachments };
            auto request = decoder.decode<HandleBrokerRequest>();
            if (request.is_error()) {
                dbgln("HandleBrokerServer: Rejecting a malformed request from process {}: {}", m_pid, request.error());
                misbehaved = true;
                return;
            }
            if (respond(serve(request.value())).is_error())
                misbehaved = true;
        });

        if (should_shutdown == Transport::ShouldShutdown::Yes || misbehaved)
            close();
    }

    // The transport is read from within its own read hook, so it can only go away once that hook has returned.
    void close()
    {
        m_is_closing = true;
        m_transport->close();
        Core::deferred_invoke([this] {
            m_server.remove_child(*this);
        });
    }

    HandleBrokerResponse serve(HandleBrokerRequest const& request)
    {
        HandleBrokerResponse response;
        response.operation = request.operation;

        auto result = [&]() -> ErrorOr<void> {
            switch (request.operation) {
            case HandleBrokerOperation::Park: {
                auto attachments = TRY(copy_handles_out_of_child(request.handle_values));
                response.token = TRY(m_server.m_broker.park_on_behalf_of(m_pid, request.channel, move(attachments)));
                return {};
            }
            case HandleBrokerOperation::Redeem: {
                auto attachments = TRY(m_server.m_broker.redeem(request.token));
                response.handle_values = TRY(copy_handles_into_child(attachments));
                return {};
            }
            case HandleBrokerOperation::ForgetChannel:
                m_server.m_broker.forget_channel_of(m_pid, request.channel);
                return {};
            }
            VERIFY_NOT_REACHED();
        }();

        if (result.is_error()) {
            auto error = result.release_error();
            response.error = error.is_errno() ? error.code() : EINVAL;
        }
        return response;
    }

    // Winsock documents WSADuplicateSocket for sockets, which needs the target process to be known in advance. A socket
    // of a base provider is a plain kernel handle that Winsock takes up on first use, as it does for the inherited
    // socket every helper starts with, and this process never uses the copies it holds.
    ErrorOr<Vector<Attachment>> copy_handles_out_of_child(Vector<u64> const& handle_values)
    {
        Vector<Attachment> attachments;
        TRY(attachments.try_ensure_capacity(handle_values.size()));
        for (auto value : handle_values) {
            HANDLE copied_handle = nullptr;
            if (!DuplicateHandle(m_process_handle, to_handle(static_cast<int>(value)), GetCurrentProcess(), &copied_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return Error::from_errno(EBADF);
            attachments.unchecked_append(Attachment::from_fd(to_fd(copied_handle)));
        }
        return attachments;
    }

    ErrorOr<Vector<u64>> copy_handles_into_child(Vector<Attachment> const& attachments)
    {
        Vector<u64> handle_values;
        TRY(handle_values.try_ensure_capacity(attachments.size()));
        for (auto const& attachment : attachments) {
            HANDLE copied_handle = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), to_handle(attachment.fd()), m_process_handle, &copied_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return Error::from_windows_error();
            handle_values.unchecked_append(static_cast<u64>(to_fd(copied_handle)));
        }
        return handle_values;
    }

    ErrorOr<void> respond(HandleBrokerResponse const& response)
    {
        MessageBuffer buffer;
        Encoder encoder(buffer);
        TRY(encoder.encode(response));
        return buffer.transfer_message(*m_transport);
    }

    HandleBrokerServer& m_server;
    pid_t m_pid { -1 };
    NonnullOwnPtr<Transport> m_transport;
    HANDLE m_process_handle { nullptr };
    bool m_is_closing { false };
};

NonnullOwnPtr<HandleBrokerServer> HandleBrokerServer::create()
{
    auto& broker = HandleBroker::the();
    VERIFY(broker.is_local());
    auto server = adopt_own(*new HandleBrokerServer(static_cast<LocalHandleBroker&>(broker)));

    MutexLocker locker(server->m_mutex);
    server->m_condition.wait_while([&] { return !server->m_event_loop; });
    return server;
}

HandleBrokerServer::HandleBrokerServer(LocalHandleBroker& broker)
    : m_broker(broker)
    , m_thread(Threading::Thread::construct("Handle broker"sv, [this] {
        return thread_main();
    }))
{
    m_thread->start();
}

HandleBrokerServer::~HandleBrokerServer()
{
    if (auto event_loop = m_event_loop->take()) {
        event_loop->quit(0);
        event_loop->wake();
    }
    (void)m_thread->join();
}

intptr_t HandleBrokerServer::thread_main()
{
    Core::EventLoop event_loop;
    {
        MutexLocker locker(m_mutex);
        m_event_loop = Core::EventLoop::current_weak();
        m_condition.broadcast();
    }

    auto result = event_loop.exec();
    m_children.clear();
    return result;
}

// What the child parked stays: a message it wrote before it went may still be unread in a live socket, and the reader
// of a channel whose writer is gone redeems what it reads, as a descriptor in flight outlives its sender elsewhere.
void HandleBrokerServer::remove_child(HandleBrokerChildConnection& child)
{
    m_children.remove_first_matching([&](auto const& entry) { return entry.ptr() == &child; });
}

void HandleBrokerServer::add_child(pid_t pid, int channel_handle)
{
    HANDLE process_handle = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process_handle) {
        dbgln("HandleBrokerServer: Could not open process {} to serve it handles: {}", pid, Error::from_windows_error());
        (void)Core::System::close(channel_handle);
        return;
    }

    auto event_loop = m_event_loop->take();
    VERIFY(event_loop);

    // A transport is read on the thread that constructs it, so the broker thread constructs this one.
    event_loop->deferred_invoke([=, this] {
        auto transport_or_error = Transport::adopt_fd(channel_handle);
        if (transport_or_error.is_error()) {
            dbgln("HandleBrokerServer: Could not adopt the handle broker channel of process {}: {}", pid, transport_or_error.error());
            (void)Core::System::close(channel_handle);
            CloseHandle(process_handle);
            return;
        }

        auto transport = transport_or_error.release_value();
        // Every other process waits on this thread, so one that stops reading its answers is dropped, not waited for.
        transport->set_disconnects_when_peer_stops_reading(true);
        auto child = make<HandleBrokerChildConnection>(*this, pid, move(transport), process_handle);
        child->start();
        m_children.append(move(child));
    });
}

}
