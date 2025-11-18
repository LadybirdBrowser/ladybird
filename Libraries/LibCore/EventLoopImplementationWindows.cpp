/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Diagnostics.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <AK/Windows.h>
#include <LibCore/EventLoopImplementationWindows.h>
#include <LibCore/Notifier.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibCore/Timer.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/MutexProtected.h>
#include <LibThreading/RWLock.h>
#include <pthread.h>

struct OwnHandle {
    HANDLE handle = NULL;

    explicit OwnHandle(HANDLE h = NULL)
        : handle(h)
    {
    }

    OwnHandle(OwnHandle&& h)
    {
        handle = h.handle;
        h.handle = NULL;
    }

    // This operation can only be done when handle is NULL
    OwnHandle& operator=(OwnHandle&& other)
    {
        VERIFY(!handle);
        if (this == &other)
            return *this;
        handle = other.handle;
        other.handle = NULL;
        return *this;
    }

    ~OwnHandle()
    {
        if (handle)
            CloseHandle(handle);
    }

    bool operator==(OwnHandle const& h) const { return handle == h.handle; }
    bool operator==(HANDLE h) const { return handle == h; }
};

template<>
struct Traits<OwnHandle> : DefaultTraits<OwnHandle> {
    static unsigned hash(OwnHandle const& h) { return Traits<HANDLE>::hash(h.handle); }
};
template<>
constexpr bool IsHashCompatible<HANDLE, OwnHandle> = true;

namespace Core {

enum class CompletionType : u8 {
    Wake,
    Timer,
    Notifier,
};

struct CompletionPacket {
    CompletionType type;
};

struct EventLoopTimer final : CompletionPacket {

    ~EventLoopTimer()
    {
        CancelWaitableTimer(timer.handle);
    }

    OwnHandle timer;
    OwnHandle wait_packet;
    WeakPtr<EventReceiver> owner;
    bool is_periodic;
};

struct EventLoopNotifier final : CompletionPacket {

    ~EventLoopNotifier() = default;

    Notifier::Type notifier_type() const { return m_notifier_type; }

    // These are a space tradeoff for avoiding a double indirection through the notifier*.
    Notifier* notifier;
    Notifier::Type m_notifier_type;
    int fd;
    OwnHandle wait_packet;
    OwnHandle wait_event;
};

struct ThreadData;

namespace {

thread_local pthread_t s_thread_id {};
HashMap<u32, ThreadData*> s_thread_data;
Threading::RWLock s_thread_data_lock;

}

static Threading::MutexProtected<HashMap<intptr_t, NonnullOwnPtr<EventLoopTimer>>> s_timers;

struct ThreadData {
    static ThreadData* the()
    {
        if (s_thread_id.p == 0) {
            s_thread_id = pthread_self();
        }
        thread_local ThreadData* thread_data_ptr = nullptr;
        if (!thread_data_ptr) {
            thread_local OwnPtr<ThreadData> thread_data = make<ThreadData>();
            thread_data_ptr = thread_data.ptr();
            Threading::RWLockLocker<Threading::LockMode::Write> lock(s_thread_data_lock);
            s_thread_data.set(pthread_getw32threadid_np(s_thread_id), thread_data.ptr());
        }
        return thread_data_ptr;
    }

    // The thread data lock should be held by the caller
    static ThreadData* for_thread(pthread_t thread)
    {
        return s_thread_data.get(pthread_getw32threadid_np(thread)).value_or(nullptr);
    }

    ThreadData()
        : wake_completion_key(make<CompletionPacket>(CompletionType::Wake))
    {
        // Consider a way for different event loops to have a different number of threads
        iocp.handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
        VERIFY(iocp.handle);
    }

    ~ThreadData()
    {
        Threading::RWLockLocker<Threading::LockMode::Write> locker(s_thread_data_lock);
        s_thread_data.remove(pthread_getw32threadid_np(s_thread_id));
    }

    OwnHandle iocp;

    // These are only used to register and unregister. The event loop doesn't access these.
    HashMap<Notifier*, NonnullOwnPtr<EventLoopNotifier>> notifiers;

    // The wake completion key is posted to the thread's event loop to wake it.
    NonnullOwnPtr<CompletionPacket> wake_completion_key;

    Threading::Mutex mutex;
};

EventLoopImplementationWindows::EventLoopImplementationWindows()
    : m_wake_completion_key((void*)ThreadData::the()->wake_completion_key.ptr())
{
}

EventLoopImplementationWindows::~EventLoopImplementationWindows()
{
}

int EventLoopImplementationWindows::exec()
{
    for (;;) {
        if (m_exit_requested)
            return m_exit_code;
        pump(PumpMode::WaitForEvents);
    }
    VERIFY_NOT_REACHED();
}

static constexpr bool debug_event_loop = false;

size_t EventLoopImplementationWindows::pump(PumpMode pump_mode)
{
    auto& event_queue = ThreadEventQueue::current();
    auto* thread_data = ThreadData::the();
    NTSTATUS status {};

    // NOTE: The number of entries to dequeue is to be optimized. Ideally we always dequeue all outstanding packets,
    // but we don't want to increase the cost of each pump unnecessarily. If more than one entry is never dequeued
    // at once, we could switch to using GetQueuedCompletionStatus which directly returns the values.

    constexpr ULONG entry_count = 32;
    OVERLAPPED_ENTRY entries[entry_count];
    ULONG entries_removed = 0;

    bool has_pending_events = event_queue.has_pending_events();
    DWORD timeout = 0;
    if (!has_pending_events && pump_mode == PumpMode::WaitForEvents)
        timeout = INFINITE;

    dbgln_if(debug_event_loop, "Waiting for events on thread {}", GetCurrentThreadId());
    BOOL success = GetQueuedCompletionStatusEx(thread_data->iocp.handle, entries, entry_count, &entries_removed, timeout, FALSE);
    dbgln_if(debug_event_loop, "Event loop dequed {} events on thread {}", entries_removed, GetCurrentThreadId());

    if (success) {
        for (ULONG i = 0; i < entries_removed; i++) {
            auto& entry = entries[i];
            auto* packet = reinterpret_cast<CompletionPacket*>(entry.lpCompletionKey);

            if (packet == thread_data->wake_completion_key) {
                dbgln_if(debug_event_loop, "Event loop on thread {} was woken", GetCurrentThreadId());
                continue;
            }
            if (packet->type == CompletionType::Timer) {
                auto* timer = static_cast<EventLoopTimer*>(packet);
                if (auto owner = timer->owner.strong_ref())
                    event_queue.post_event(*owner, make<TimerEvent>());
                if (timer->is_periodic) {
                    status = g_system.NtAssociateWaitCompletionPacket(timer->wait_packet.handle, thread_data->iocp.handle, timer->timer.handle, timer, NULL, 0, 0, NULL);
                    VERIFY(NT_SUCCESS(status));
                }
                dbgln_if(debug_event_loop, "Dequeud a timer on thread {}", GetCurrentThreadId());
                continue;
            }
            if (packet->type == CompletionType::Notifier) {
                auto* notifier_data = reinterpret_cast<EventLoopNotifier*>(packet);
                event_queue.post_event(*notifier_data->notifier, make<NotifierActivationEvent>(notifier_data->fd, notifier_data->notifier_type()));
                status = g_system.NtAssociateWaitCompletionPacket(notifier_data->wait_packet.handle, thread_data->iocp.handle, notifier_data->wait_event.handle, notifier_data, NULL, 0, 0, NULL);
                VERIFY(NT_SUCCESS(status));
                dbgln_if(debug_event_loop, "Dequed a notifier on thread {}", GetCurrentThreadId());
                continue;
            }
            VERIFY_NOT_REACHED();
        }
    } else {
        DWORD error = GetLastError();
        switch (error) {
        case WAIT_TIMEOUT:
            break;
        default:
            dbgln("GetQueuedCompletionStatusEx failed with unexpected error: {}", Error::from_windows_error(error));
            VERIFY_NOT_REACHED();
        }
    }

    return event_queue.process();
}

void EventLoopImplementationWindows::quit(int code)
{
    m_exit_requested = true;
    m_exit_code = code;
}

void EventLoopImplementationWindows::post_event(EventReceiver& receiver, NonnullOwnPtr<Event>&& event)
{
    m_thread_event_queue.post_event(receiver, move(event));
    if (&m_thread_event_queue != &ThreadEventQueue::current())
        wake();
}

void EventLoopImplementationWindows::wake()
{
    auto* thread_data = ThreadData::the();
    PostQueuedCompletionStatus(thread_data->iocp.handle, 0, (ULONG_PTR)m_wake_completion_key, NULL);
}

static int notifier_type_to_network_event(NotificationType type)
{
    switch (type) {
    case NotificationType::Read:
        return FD_READ | FD_CLOSE | FD_ACCEPT;
    case NotificationType::Write:
        return FD_WRITE;
    default:
        dbgln("This notification type is not implemented: {}", (int)type);
        VERIFY_NOT_REACHED();
    }
}

void EventLoopManagerWindows::register_notifier(Notifier& notifier)
{
    auto* thread_data = ThreadData::the();
    VERIFY(thread_data);

    Threading::MutexLocker lock(thread_data->mutex);
    auto& notifiers = thread_data->notifiers;

    if (notifiers.contains(&notifier))
        return;

    HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);
    VERIFY(event);
    int rc = WSAEventSelect(notifier.fd(), event, notifier_type_to_network_event(notifier.type()));
    VERIFY(!rc);

    auto notifier_data = make<EventLoopNotifier>();
    notifier_data->type = CompletionType::Notifier;
    notifier_data->notifier = &notifier;
    notifier_data->m_notifier_type = notifier.type();
    notifier_data->wait_event.handle = event;
    notifier_data->fd = notifier.fd();
    NTSTATUS status = NtCreateWaitCompletionPacket(&notifier_data->wait_packet.handle, GENERIC_READ | GENERIC_WRITE, NULL);
    VERIFY(NT_SUCCESS(status));
    status = NtAssociateWaitCompletionPacket(notifier_data->wait_packet.handle, thread_data->iocp.handle, event, notifier_data.ptr(), NULL, 0, 0, NULL);
    VERIFY(NT_SUCCESS(status));
    notifiers.set(&notifier, move(notifier_data));
    notifier.set_owner_thread(s_thread_id);
    dbgln_if(debug_event_loop, "Registered notifier: {}", &notifier);
}

void EventLoopManagerWindows::unregister_notifier(Notifier& notifier)
{
    Threading::RWLockLocker<Threading::LockMode::Read> thread_data_lock(s_thread_data_lock);
    auto* thread_data = ThreadData::for_thread(notifier.owner_thread());
    VERIFY(thread_data);
    dbgln_if(debug_event_loop, "Unregistered notifier: {}; for thread {}", &notifier, pthread_getw32threadid_np(notifier.owner_thread()));

    Threading::MutexLocker lock(thread_data->mutex);
    VERIFY(pthread_equal(pthread_self(), notifier.owner_thread()));

    auto& notifiers = thread_data->notifiers;
    auto maybe_notifier_data = notifiers.take(&notifier);
    if (!maybe_notifier_data.has_value())
        return;
    auto notifier_data = move(maybe_notifier_data.value());
    // We are removing the signalled packets since the caller no longer expects them
    NTSTATUS status = g_system.NtCancelWaitCompletionPacket(notifier_data->wait_packet.handle, TRUE);
    VERIFY(NT_SUCCESS(status));
    // TODO: Reuse the data structure
}

intptr_t EventLoopManagerWindows::register_timer(EventReceiver& object, int milliseconds, bool should_reload)
{
    VERIFY(milliseconds >= 0);
    auto* thread_data = ThreadData::the();
    VERIFY(thread_data);

    // Only needed to make reading handles safe
    Threading::MutexLocker lock(thread_data->mutex);

    auto timer_data = make<EventLoopTimer>();
    timer_data->type = CompletionType::Timer;
    timer_data->timer.handle = CreateWaitableTimer(NULL, FALSE, NULL);
    timer_data->owner = object.make_weak_ptr();
    timer_data->is_periodic = should_reload;
    VERIFY(timer_data->timer.handle);

    NTSTATUS status = g_system.NtCreateWaitCompletionPacket(&timer_data->wait_packet.handle, GENERIC_READ | GENERIC_WRITE, NULL);
    VERIFY(NT_SUCCESS(status));

    LARGE_INTEGER first_time = {};
    // Measured in 0.1μs intervals, negative means starting from now
    first_time.QuadPart = -10'000LL * milliseconds;
    BOOL succeeded = SetWaitableTimer(timer_data->timer.handle, &first_time, should_reload ? milliseconds : 0, NULL, NULL, FALSE);
    VERIFY(succeeded);

    status = g_system.NtAssociateWaitCompletionPacket(timer_data->wait_packet.handle, thread_data->iocp.handle, timer_data->timer.handle, timer_data.ptr(), NULL, 0, 0, NULL);
    VERIFY(NT_SUCCESS(status));

    auto timer_id = reinterpret_cast<intptr_t>(timer_data.ptr());
    s_timers.with_locked([&timer_id, &timer_data](auto& map) { map.set(timer_id, move(timer_data)); });
    return timer_id;
}

void EventLoopManagerWindows::unregister_timer(intptr_t timer_id)
{

    auto maybe_timer = s_timers.with_locked([&timer_id](auto& map) { return map.take(timer_id); });
    if (!maybe_timer.has_value())
        VERIFY_NOT_REACHED();
    auto timer = move(maybe_timer.value());

    g_system.NtCancelWaitCompletionPacket(timer->wait_packet.handle, TRUE);
}

int EventLoopManagerWindows::register_signal([[maybe_unused]] int signal_number, [[maybe_unused]] Function<void(int)> handler)
{
    dbgln("Core::EventLoopManagerWindows::register_signal() is not implemented");
    VERIFY_NOT_REACHED();
}

void EventLoopManagerWindows::unregister_signal([[maybe_unused]] int handler_id)
{
    dbgln("Core::EventLoopManagerWindows::unregister_signal() is not implemented");
    VERIFY_NOT_REACHED();
}

void EventLoopManagerWindows::did_post_event()
{
}

NonnullOwnPtr<EventLoopImplementation> EventLoopManagerWindows::make_implementation()
{
    return make<EventLoopImplementationWindows>();
}

}
