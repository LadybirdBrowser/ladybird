/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <LibCore/Socket.h>

#if defined(AK_OS_WINDOWS)
#    include <LibIPC/TransportWindows.h>
#else
#    include <AK/ByteBuffer.h>
#    include <AK/Function.h>
#    include <AK/NonnullOwnPtr.h>
#    include <AK/Optional.h>
#    include <AK/Queue.h>
#    include <AK/RefPtr.h>
#    include <AK/Time.h>
#    include <AK/Vector.h>
#    include <AK/kmalloc.h>
#    include <LibCore/Forward.h>
#    include <LibIPC/Attachment.h>
#    include <LibIPC/File.h>
#    include <LibIPC/Forward.h>
#    include <LibIPC/ReceivedMessageBytes.h>
#    include <LibIPC/TransportHandle.h>
#    if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
#        include <LibCore/MachPort.h>
#    endif
#endif

namespace IPC {

#if !defined(AK_OS_WINDOWS)

namespace FFI {

struct RustTransport;

}

// The transport of LibIPC's Rust crate, over Mach ports on Darwin and a Unix domain socket elsewhere. It does the IO on
// one thread that every transport in the process shares.
class Transport {
    AK_MAKE_NONCOPYABLE(Transport);
    AK_MAKE_NONMOVABLE(Transport);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct Paired {
        NonnullOwnPtr<Transport> local;
        TransportHandle remote_handle;
    };
    static ErrorOr<Paired> create_paired();

#    if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    static ErrorOr<NonnullOwnPtr<Transport>> create(Core::MachPort receive_right, Core::MachPort send_right);
    static void raise_receive_queue_limit(Core::MachPort const&);
#    else
    static ErrorOr<NonnullOwnPtr<Transport>> from_socket(NonnullOwnPtr<Core::LocalSocket>);

    // The bytes and attachments are what the last owner read off the socket without seeing the end of the message.
    static ErrorOr<NonnullOwnPtr<Transport>> adopt_fd(int fd, ByteBuffer unprocessed_bytes = {}, Vector<Attachment> unprocessed_attachments = {});
#    endif

    ~Transport();

    void set_up_read_hook(Function<void()>);

    // Sets what the IO thread shows every message that arrives, before the transport's owner gets to see it, and empty
    // bytes once the peer is gone. This is for a thread that does not get back to its event loop on its own, like one
    // stuck in script. It runs while the IO thread holds a lock, so it should only have a look.
    void set_incoming_message_observer(Function<void(ReadonlyBytes)>);

    // False once this end is closed or asked to stop, a send has failed, or the peer is gone.
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

    // Wait until everything the peer has already sent has been taken into the incoming queue.
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

    // Whole messages that have arrived stay here for the old owner to take, and anything newer reaches the new owner.
    ErrorOr<TransportHandle> release_for_transfer();

private:
    Transport(FFI::RustTransport*, File notify_hook_read_fd);

    FFI::RustTransport* m_transport { nullptr };

    File m_notify_hook_read_fd;
    RefPtr<Core::Notifier> m_read_hook_notifier;
    Function<void()> m_on_read_hook;
};

#endif

}
