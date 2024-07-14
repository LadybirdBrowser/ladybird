/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/IDAllocator.h>
#include <AK/Singleton.h>
#include <AK/TemporaryChange.h>
#include <LibCore/Event.h>
#include <LibCore/Notifier.h>
#include <LibCore/ThreadEventQueue.h>

#import <Application/EventLoopImplementation.h>
#import <System/Cocoa.h>
#import <System/CoreFoundation.h>

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

namespace Ladybird {

struct ThreadData {
    static ThreadData& the()
    {
        static thread_local ThreadData s_thread_data;
        return s_thread_data;
    }

    Core::Notifier& notifier_by_fd(int fd)
    {
        for (auto notifier : notifiers) {
            if (notifier.key->fd() == fd)
                return *notifier.key;
        }

        // If we didn't have a notifier for the provided FD, it should have been unregistered.
        VERIFY_NOT_REACHED();
    }

    IDAllocator timer_id_allocator;
    HashMap<int, CFRunLoopTimerRef> timers;
    HashMap<Core::Notifier*, CFRunLoopSourceRef> notifiers;
};

class SignalHandlers : public RefCounted<SignalHandlers> {
    AK_MAKE_NONCOPYABLE(SignalHandlers);
    AK_MAKE_NONMOVABLE(SignalHandlers);

public:
    SignalHandlers(int signal_number, CFFileDescriptorCallBack);
    ~SignalHandlers();

    void dispatch();
    int add(Function<void(int)>&& handler);
    bool remove(int handler_id);

    bool is_empty() const
    {
        if (m_calling_handlers) {
            for (auto const& handler : m_handlers_pending) {
                if (handler.value)
                    return false; // an add is pending
            }
        }
        return m_handlers.is_empty();
    }

    bool have(int handler_id) const
    {
        if (m_calling_handlers) {
            auto it = m_handlers_pending.find(handler_id);
            if (it != m_handlers_pending.end()) {
                if (!it->value)
                    return false; // a deletion is pending
            }
        }
        return m_handlers.contains(handler_id);
    }

    int m_signal_number;
    void (*m_original_handler)(int);
    HashMap<int, Function<void(int)>> m_handlers;
    HashMap<int, Function<void(int)>> m_handlers_pending;
    bool m_calling_handlers { false };
    CFRunLoopSourceRef m_source { nullptr };
    int m_kevent_fd = { -1 };
};

SignalHandlers::SignalHandlers(int signal_number, CFFileDescriptorCallBack handle_signal)
    : m_signal_number(signal_number)
    , m_original_handler(signal(signal_number, [](int) {}))
{
    m_kevent_fd = kqueue();
    if (m_kevent_fd < 0) {
        dbgln("Unable to create kqueue to register signal {}: {}", signal_number, strerror(errno));
        VERIFY_NOT_REACHED();
    }

    struct kevent changes = {};
    EV_SET(&changes, signal_number, EVFILT_SIGNAL, EV_ADD | EV_RECEIPT, 0, 0, nullptr);
    if (auto res = kevent(m_kevent_fd, &changes, 1, &changes, 1, NULL); res < 0) {
        dbgln("Unable to register signal {}: {}", signal_number, strerror(errno));
        VERIFY_NOT_REACHED();
    }

    CFFileDescriptorContext context = { 0, this, nullptr, nullptr, nullptr };
    CFFileDescriptorRef kq_ref = CFFileDescriptorCreate(kCFAllocatorDefault, m_kevent_fd, FALSE, handle_signal, &context);

    m_source = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, kq_ref, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), m_source, kCFRunLoopDefaultMode);

    CFFileDescriptorEnableCallBacks(kq_ref, kCFFileDescriptorReadCallBack);
    CFRelease(kq_ref);
}

SignalHandlers::~SignalHandlers()
{
    CFRunLoopRemoveSource(CFRunLoopGetMain(), m_source, kCFRunLoopDefaultMode);
    CFRelease(m_source);
    (void)::signal(m_signal_number, m_original_handler);
    ::close(m_kevent_fd);
}

struct SignalHandlersInfo {
    HashMap<int, NonnullRefPtr<SignalHandlers>> signal_handlers;
    int next_signal_id { 0 };
};

static Singleton<SignalHandlersInfo> s_signals;
static SignalHandlersInfo* signals_info()
{
    return s_signals.ptr();
}

void SignalHandlers::dispatch()
{
    TemporaryChange change(m_calling_handlers, true);
    for (auto& handler : m_handlers)
        handler.value(m_signal_number);
    if (!m_handlers_pending.is_empty()) {
        // Apply pending adds/removes
        for (auto& handler : m_handlers_pending) {
            if (handler.value) {
                auto result = m_handlers.set(handler.key, move(handler.value));
                VERIFY(result == AK::HashSetResult::InsertedNewEntry);
            } else {
                m_handlers.remove(handler.key);
            }
        }
        m_handlers_pending.clear();
    }
}

int SignalHandlers::add(Function<void(int)>&& handler)
{
    int id = ++signals_info()->next_signal_id; // TODO: worry about wrapping and duplicates?
    if (m_calling_handlers)
        m_handlers_pending.set(id, move(handler));
    else
        m_handlers.set(id, move(handler));
    return id;
}

bool SignalHandlers::remove(int handler_id)
{
    VERIFY(handler_id != 0);
    if (m_calling_handlers) {
        auto it = m_handlers.find(handler_id);
        if (it != m_handlers.end()) {
            // Mark pending remove
            m_handlers_pending.set(handler_id, {});
            return true;
        }
        it = m_handlers_pending.find(handler_id);
        if (it != m_handlers_pending.end()) {
            if (!it->value)
                return false; // already was marked as deleted
            it->value = nullptr;
            return true;
        }
        return false;
    }
    return m_handlers.remove(handler_id);
}

static void post_application_event()
{
    auto* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                     location:NSMakePoint(0, 0)
                                modifierFlags:0
                                    timestamp:0
                                 windowNumber:0
                                      context:nil
                                      subtype:0
                                        data1:0
                                        data2:0];

    [NSApp postEvent:event atStart:NO];
}

NonnullOwnPtr<Core::EventLoopImplementation> CFEventLoopManager::make_implementation()
{
    return CFEventLoopImplementation::create();
}

intptr_t CFEventLoopManager::register_timer(Core::EventReceiver& receiver, int interval_milliseconds, bool should_reload, Core::TimerShouldFireWhenNotVisible should_fire_when_not_visible)
{
    auto& thread_data = ThreadData::the();

    auto timer_id = thread_data.timer_id_allocator.allocate();
    auto weak_receiver = receiver.make_weak_ptr();

    auto interval_seconds = static_cast<double>(interval_milliseconds) / 1000.0;
    auto first_fire_time = CFAbsoluteTimeGetCurrent() + interval_seconds;

    auto* timer = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, first_fire_time, should_reload ? interval_seconds : 0, 0, 0,
        ^(CFRunLoopTimerRef) {
            auto receiver = weak_receiver.strong_ref();
            if (!receiver) {
                return;
            }

            if (should_fire_when_not_visible == Core::TimerShouldFireWhenNotVisible::No) {
                if (!receiver->is_visible_for_timer_purposes()) {
                    return;
                }
            }

            Core::TimerEvent event;
            receiver->dispatch_event(event);
        });

    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
    thread_data.timers.set(timer_id, timer);

    return timer_id;
}

void CFEventLoopManager::unregister_timer(intptr_t timer_id)
{
    auto& thread_data = ThreadData::the();
    thread_data.timer_id_allocator.deallocate(static_cast<int>(timer_id));

    auto timer = thread_data.timers.take(static_cast<int>(timer_id));
    VERIFY(timer.has_value());
    CFRunLoopTimerInvalidate(*timer);
    CFRelease(*timer);
}

static void socket_notifier(CFSocketRef socket, CFSocketCallBackType notification_type, CFDataRef, void const*, void*)
{
    auto& notifier = ThreadData::the().notifier_by_fd(CFSocketGetNative(socket));

    // This socket callback is not quite re-entrant. If Core::Notifier::dispatch_event blocks, e.g.
    // to wait upon a Core::Promise, this socket will not receive any more notifications until that
    // promise is resolved or rejected. So we mark this socket as able to receive more notifications
    // before dispatching the event, which allows it to be triggered again.
    CFSocketEnableCallBacks(socket, notification_type);

    Core::NotifierActivationEvent event(notifier.fd(), notifier.type());
    notifier.dispatch_event(event);

    // This manual process of enabling the callbacks also seems to require waking the event loop,
    // otherwise it hangs indefinitely in any ongoing pump(PumpMode::WaitForEvents) invocation.
    post_application_event();
}

void CFEventLoopManager::register_notifier(Core::Notifier& notifier)
{
    auto notification_type = kCFSocketNoCallBack;

    switch (notifier.type()) {
    case Core::Notifier::Type::Read:
        notification_type = kCFSocketReadCallBack;
        break;
    case Core::Notifier::Type::Write:
        notification_type = kCFSocketWriteCallBack;
        break;
    default:
        TODO();
        break;
    }

    CFSocketContext context { .version = 0, .info = nullptr, .retain = nullptr, .release = nullptr, .copyDescription = nullptr };
    auto* socket = CFSocketCreateWithNative(kCFAllocatorDefault, notifier.fd(), notification_type, &socket_notifier, &context);

    CFOptionFlags sockopt = CFSocketGetSocketFlags(socket);
    sockopt &= ~kCFSocketAutomaticallyReenableReadCallBack;
    sockopt &= ~kCFSocketCloseOnInvalidate;
    CFSocketSetSocketFlags(socket, sockopt);

    auto* source = CFSocketCreateRunLoopSource(kCFAllocatorDefault, socket, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);

    CFRelease(socket);

    ThreadData::the().notifiers.set(&notifier, source);
}

void CFEventLoopManager::unregister_notifier(Core::Notifier& notifier)
{
    if (auto source = ThreadData::the().notifiers.take(&notifier); source.has_value()) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), *source, kCFRunLoopDefaultMode);
        CFRelease(*source);
    }
}

void CFEventLoopManager::did_post_event()
{
    post_application_event();
}

static void handle_signal(CFFileDescriptorRef f, CFOptionFlags callback_types, void* info)
{
    VERIFY(callback_types & kCFFileDescriptorReadCallBack);
    auto* signal_handlers = static_cast<SignalHandlers*>(info);

    struct kevent event { };

    // returns number of events that have occurred since last call
    (void)::kevent(CFFileDescriptorGetNativeDescriptor(f), nullptr, 0, &event, 1, nullptr);
    CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);

    signal_handlers->dispatch();
}

int CFEventLoopManager::register_signal(int signal_number, Function<void(int)> handler)
{
    VERIFY(signal_number != 0);
    auto& info = *signals_info();
    auto handlers = info.signal_handlers.find(signal_number);
    if (handlers == info.signal_handlers.end()) {
        auto signal_handlers = adopt_ref(*new SignalHandlers(signal_number, &handle_signal));
        auto handler_id = signal_handlers->add(move(handler));
        info.signal_handlers.set(signal_number, move(signal_handlers));
        return handler_id;
    } else {
        return handlers->value->add(move(handler));
    }
}

void CFEventLoopManager::unregister_signal(int handler_id)
{
    VERIFY(handler_id != 0);
    int remove_signal_number = 0;
    auto& info = *signals_info();
    for (auto& h : info.signal_handlers) {
        auto& handlers = *h.value;
        if (handlers.remove(handler_id)) {
            if (handlers.is_empty())
                remove_signal_number = handlers.m_signal_number;
            break;
        }
    }
    if (remove_signal_number != 0)
        info.signal_handlers.remove(remove_signal_number);
}

NonnullOwnPtr<CFEventLoopImplementation> CFEventLoopImplementation::create()
{
    return adopt_own(*new CFEventLoopImplementation);
}

int CFEventLoopImplementation::exec()
{
    [NSApp run];
    return m_exit_code;
}

size_t CFEventLoopImplementation::pump(PumpMode mode)
{
    auto* wait_until = mode == PumpMode::WaitForEvents ? [NSDate distantFuture] : [NSDate distantPast];

    auto* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                     untilDate:wait_until
                                        inMode:NSDefaultRunLoopMode
                                       dequeue:YES];

    while (event) {
        [NSApp sendEvent:event];

        event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                   untilDate:nil
                                      inMode:NSDefaultRunLoopMode
                                     dequeue:YES];
    }

    return 0;
}

void CFEventLoopImplementation::quit(int exit_code)
{
    m_exit_code = exit_code;
    [NSApp stop:nil];
}

void CFEventLoopImplementation::wake()
{
    CFRunLoopWakeUp(CFRunLoopGetCurrent());
}

void CFEventLoopImplementation::post_event(Core::EventReceiver& receiver, NonnullOwnPtr<Core::Event>&& event)
{
    m_thread_event_queue.post_event(receiver, move(event));

    if (&m_thread_event_queue != &Core::ThreadEventQueue::current())
        wake();
}

}
