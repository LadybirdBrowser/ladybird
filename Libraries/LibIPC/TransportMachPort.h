/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#if !defined(AK_OS_MACOS) && !defined(AK_OS_IOS)
#    error "TransportMachPort is only available on Darwin"
#endif

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibCore/Forward.h>
#include <LibCore/MachPort.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/AutoCloseFileDescriptor.h>
#include <LibIPC/Forward.h>
#include <LibIPC/ReceivedMessageBytes.h>
#include <LibIPC/TransportHandle.h>

namespace IPC {

namespace FFI {

struct MachTransport;

}

// The Mach port transport of LibIPC's Rust crate, which does the IO on one thread that every transport in the process
// shares.
class TransportMachPort {
    AK_MAKE_NONCOPYABLE(TransportMachPort);
    AK_MAKE_NONMOVABLE(TransportMachPort);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct Paired {
        NonnullOwnPtr<TransportMachPort> local;
        TransportHandle remote_handle;
    };
    static ErrorOr<Paired> create_paired();
    static ErrorOr<NonnullOwnPtr<TransportMachPort>> create(Core::MachPort receive_right, Core::MachPort send_right);

    static void raise_receive_queue_limit(Core::MachPort const&);

    ~TransportMachPort();

    void set_up_read_hook(Function<void()>);

    // Sets what the IO thread shows every message that arrives, before the transport's owner gets to see it, and empty
    // bytes once the peer is gone. This is for a thread that does not get back to its event loop on its own, like one
    // stuck in script. It runs while the IO thread holds a lock, so it should only have a look.
    void set_incoming_message_observer(Function<void(ReadonlyBytes)>);

    bool is_open() const;

    void close();
    void close_after_sending_all_pending_messages();

    // Returns at the deadline as well, if one is given.
    void wait_until_readable(Optional<MonotonicTime> deadline = {});

    // Makes wait_until_readable() return, even with nothing to read, now or if none is under way, the next time. Any
    // thread may call this.
    void interrupt_wait_until_readable();

    // Wait until everything posted so far has been handed to the kernel.
    void flush();

    // Wait until everything the peer has already sent has been parsed into the incoming queue.
    void wait_until_incoming_is_current();

    ErrorOr<void> post_message(MessageDataType, Vector<Attachment>& attachments);

    enum class ShouldShutdown {
        No,
        Yes,
    };
    struct Message {
        AK_ALLOC_WITH_KMALLOC;

        ReceivedMessageBytes bytes;
        Queue<Attachment> attachments;
    };
    ShouldShutdown read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&&);

    ErrorOr<TransportHandle> release_for_transfer();

private:
    TransportMachPort(FFI::MachTransport*, NonnullRefPtr<AutoCloseFileDescriptor> notify_hook_read_fd);

    FFI::MachTransport* m_transport { nullptr };

    RefPtr<AutoCloseFileDescriptor> m_notify_hook_read_fd;
    RefPtr<Core::Notifier> m_read_hook_notifier;
    Function<void()> m_on_read_hook;
};

}
