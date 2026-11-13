/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Types.h>
#include <Compositor/MainDispatchQueue.h>
#include <LibCore/Notifier.h>

#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/event.h>
#include <unistd.h>

namespace Compositor {

// libdispatch wakes the main thread's run loop through a port of its own, and the run loop has libdispatch drain the
// queue once the port has a message. Neither function is public, as CoreFoundation is their only intended caller.
using MainQueuePortFunction = mach_port_t (*)();
using MainQueueDrainFunction = void (*)(void*);

ErrorOr<void> drain_main_dispatch_queue_in_event_loop()
{
    VERIFY(pthread_main_np());

    auto main_queue_port = reinterpret_cast<MainQueuePortFunction>(dlsym(RTLD_DEFAULT, "_dispatch_get_main_queue_port_4CF"));
    auto drain_main_queue = reinterpret_cast<MainQueueDrainFunction>(dlsym(RTLD_DEFAULT, "_dispatch_main_queue_callback_4CF"));
    if (!main_queue_port || !drain_main_queue)
        return Error::from_string_literal("libdispatch has no functions to drain its main queue with");
    auto port = main_queue_port();
    if (port == MACH_PORT_NULL)
        return Error::from_string_literal("libdispatch has no port for its main queue");

    // A kqueue reads as ready for as long as the port holds a message, so the event loop can wait on it like a socket.
    auto port_queue = kqueue();
    if (port_queue < 0)
        return Error::from_syscall("kqueue"sv, errno);
    struct kevent change;
    EV_SET(&change, port, EVFILT_MACHPORT, EV_ADD, 0, 0, nullptr);
    if (kevent(port_queue, &change, 1, nullptr, 0, nullptr) < 0) {
        auto error = Error::from_syscall("kevent"sv, errno);
        close(port_queue);
        return error;
    }

    auto notifier = Core::Notifier::construct(port_queue, Core::Notifier::Type::Read);
    notifier->on_activation = [port, port_queue, drain_main_queue] {
        struct kevent event;
        timespec const no_wait {};
        (void)kevent(port_queue, nullptr, 0, &event, 1, &no_wait);

        // The message only wakes the run loop, and the run loop takes it off the port before libdispatch drains.
        struct {
            mach_msg_header_t header;
            u8 body_and_trailer[256];
        } message;
        while (mach_msg(&message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(message), port, 0, MACH_PORT_NULL) == MACH_MSG_SUCCESS) { }
        // Blocks on the main queue autorelease into the run loop's pool, which this process doesn't have.
        @autoreleasepool {
            drain_main_queue(nullptr);
        }
    };
    // The queue is drained for as long as the process lives.
    (void)notifier.leak_ref();
    return {};
}

}
