/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Queue.h>
#include <AK/kmalloc.h>
#include <LibCore/System.h>
#include <LibIPC/RustTransportWindows.h>
#include <LibIPC/StartupTransport.h>
#include <LibIPC/Transport.h>

namespace IPC {

struct RustWindowsMessage {
    AK_ALLOC_WITH_KMALLOC;

    explicit RustWindowsMessage(Transport::Message&& message)
        : bytes(move(message.bytes))
    {
        while (!message.attachments.is_empty())
            attachments.enqueue(message.attachments.dequeue());
    }

    ReceivedMessageBytes bytes;
    Queue<Attachment> attachments;
};

struct RustWindowsTransport {
    AK_ALLOC_WITH_KMALLOC;

    NonnullOwnPtr<Transport> transport;
    Queue<NonnullOwnPtr<RustWindowsMessage>> received;
    Atomic<bool> eof { false };
};

}

IPC::RustWindowsTransport* ipc_windows_transport_adopt(int descriptor)
{
    auto transport = IPC::Transport::adopt_fd(descriptor);
    if (transport.is_error()) {
        (void)Core::System::close(descriptor);
        return nullptr;
    }
    return new IPC::RustWindowsTransport { transport.release_value(), {}, false };
}

IPC::RustWindowsTransport* ipc_windows_transport_take_over()
{
    auto transport = IPC::take_over_startup_transport({});
    if (transport.is_error()) {
        warnln("Rust IPC: {}", transport.error());
        return nullptr;
    }
    return new IPC::RustWindowsTransport { transport.release_value(), {}, false };
}

bool ipc_windows_transport_pair(IPC::RustWindowsTransport** local, int* remote)
{
    auto pair = Core::System::duplex_pipe();
    if (pair.is_error())
        return false;
    *remote = pair.value()[1];
    *local = ipc_windows_transport_adopt(pair.value()[0]);
    if (!*local) {
        (void)Core::System::close(*remote);
        return false;
    }
    return true;
}

void ipc_windows_transport_destroy(IPC::RustWindowsTransport* transport)
{
    delete transport;
}

void ipc_windows_transport_close(IPC::RustWindowsTransport* transport)
{
    transport->transport->close();
}

bool ipc_windows_transport_is_open(IPC::RustWindowsTransport const* transport)
{
    return !transport->eof.load() && transport->transport->is_open();
}

bool ipc_windows_transport_send(IPC::RustWindowsTransport* transport, u8 const* bytes, size_t size, int const* descriptors, size_t descriptor_count)
{
    // Ownership of every descriptor passes to this call, even when sending fails.
    Vector<IPC::Attachment> attachments;
    for (size_t i = 0; i < descriptor_count; ++i)
        attachments.append(IPC::Attachment::from_fd(descriptors[i]));
    IPC::MessageDataType data;
    if (data.try_append(bytes, size).is_error())
        return false;
    auto result = transport->transport->post_message(move(data), attachments);
    if (result.is_error()) {
        warnln("Rust IPC: {}", result.error());
        return false;
    }
    return true;
}

IPC::RustWindowsMessage* ipc_windows_transport_receive(IPC::RustWindowsTransport* transport)
{
    while (transport->received.is_empty()) {
        if (transport->eof.load())
            return nullptr;
        auto shutdown = transport->transport->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            transport->received.enqueue(make<IPC::RustWindowsMessage>(move(message)));
        });
        if (shutdown == IPC::Transport::ShouldShutdown::Yes)
            transport->eof.store(true);
        if (!transport->received.is_empty())
            break;
        if (!transport->eof.load())
            transport->transport->wait_until_readable();
    }
    return transport->received.dequeue().leak_ptr();
}

u8 const* ipc_windows_message_bytes(IPC::RustWindowsMessage const* message, size_t* size)
{
    auto bytes = message->bytes.bytes();
    *size = bytes.size();
    return bytes.data();
}

size_t ipc_windows_message_attachment_count(IPC::RustWindowsMessage const* message)
{
    return message->attachments.size();
}

int ipc_windows_message_take_attachment(IPC::RustWindowsMessage* message)
{
    return message->attachments.dequeue().to_fd();
}

void ipc_windows_message_destroy(IPC::RustWindowsMessage* message)
{
    delete message;
}

int ipc_windows_descriptor_clone(int descriptor)
{
    auto clone = Core::System::dup(descriptor);
    if (clone.is_error())
        return -1;
    return clone.release_value();
}

void ipc_windows_descriptor_close(int descriptor)
{
    (void)Core::System::close(descriptor);
}
