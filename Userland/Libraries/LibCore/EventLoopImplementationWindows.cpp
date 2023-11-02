/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, kleines Filmröllchen <malu.bertsch@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/Singleton.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <AK/WeakPtr.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoopImplementationWindows.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <WinSock2.h>

namespace Core {

struct ThreadData;

namespace {
thread_local ThreadData* s_thread_data;
}

struct EventLoopTimer {
    int timer_id { 0 };
    Duration interval;
    MonotonicTime fire_time { MonotonicTime::now_coarse() };
    bool should_reload { false };
    TimerShouldFireWhenNotVisible fire_when_not_visible { TimerShouldFireWhenNotVisible::No };
    WeakPtr<EventReceiver> owner;

    void reload(MonotonicTime const& now) { fire_time = now + interval; }
    bool has_expired(MonotonicTime const& now) const { return now > fire_time; }
};

struct ThreadData {
    static ThreadData& the()
    {
        if (!s_thread_data) {
            // FIXME: Don't leak this.
            s_thread_data = new ThreadData;
        }
        return *s_thread_data;
    }

    ThreadData()
    {
        pid = GetCurrentProcessId();
        initialize_wake_pipe();
    }

    void initialize_wake_pipe()
    {
        SECURITY_ATTRIBUTES saAttr;
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = FALSE;
        saAttr.lpSecurityDescriptor = NULL;

        if (!CreatePipe(&wake_pipe_read, &wake_pipe_write, &saAttr, 0))
            VERIFY_NOT_REACHED();

        // Make the write end of the pipe non-inheritable
        if (!SetHandleInformation(wake_pipe_write, HANDLE_FLAG_INHERIT, 0))
            VERIFY_NOT_REACHED();
    }

    // Each thread has its own timers, notifiers and a wake pipe.
    HashMap<int, NonnullOwnPtr<EventLoopTimer>> timers;
    HashTable<Notifier*> notifiers;

    // The wake pipe is used to notify another event loop that someone has called wake(), or a signal has been received.
    // wake() writes 0i32 into the pipe, signals write the signal number (guaranteed non-zero).
    HANDLE wake_pipe_read { NULL };
    HANDLE wake_pipe_write { NULL };

    DWORD pid { 0 };

    IDAllocator id_allocator;
};

EventLoopImplementationWindows::EventLoopImplementationWindows()
    : m_wake_pipe_read_handle(ThreadData::the().wake_pipe_read)
    , m_wake_pipe_write_handle(ThreadData::the().wake_pipe_write)
{
}

EventLoopImplementationWindows::~EventLoopImplementationWindows() = default;

int EventLoopImplementationWindows::exec()
{
    dbgln("EventLoop: Execing");
    for (;;) {
        dbgln("EventLoop: Execing, waiting for events");
        if (m_exit_requested)
            return m_exit_code;
        pump(PumpMode::WaitForEvents);
    }
    VERIFY_NOT_REACHED();
}

size_t EventLoopImplementationWindows::pump(PumpMode mode)
{
    dbgln("EventLoop: pumppre");
    static_cast<EventLoopManagerWindows&>(EventLoopManager::the()).wait_for_events(mode);
    dbgln("EventLoop: pumppst");
    return ThreadEventQueue::current().process();
}

void EventLoopImplementationWindows::quit(int code)
{
    dbgln("EventLoop: Quitting with code {}", code);
    m_exit_requested = true;
    m_exit_code = code;
}

void EventLoopImplementationWindows::unquit()
{
    dbgln("EventLoop: Unquitting");
    m_exit_requested = false;
    m_exit_code = 0;
}

bool EventLoopImplementationWindows::was_exit_requested() const
{
    dbgln("EventLoop: was_exit_requested");
    return m_exit_requested;
}

void EventLoopImplementationWindows::post_event(EventReceiver& receiver, NonnullOwnPtr<Event>&& event)
{
    dbgln("EventLoop: Post event of");
    m_thread_event_queue.post_event(receiver, move(event));
    if (&m_thread_event_queue != &ThreadEventQueue::current())
        wake();
}

void EventLoopImplementationWindows::wake()
{
    dbgln("EventLoop: wake");
    int wake_event = 0;
    if (!WriteFile(m_wake_pipe_write_handle, &wake_event, sizeof(wake_event), nullptr, nullptr))
        VERIFY_NOT_REACHED();
}

void EventLoopManagerWindows::wait_for_events(EventLoopImplementation::PumpMode mode)
{
    dbgln("EventLoop: wait_for_events");
    auto& thread_data = ThreadData::the();

    WSAEVENT events[thread_data.notifiers.size()];

    SOCKET max_socket = -1;

    for (auto& notifier : thread_data.notifiers) {
        events[notifier->fd()] = WSACreateEvent();

        if (notifier->type() == Notifier::Type::Read)
            WSAEventSelect(notifier->fd(), events[notifier->fd()], FD_READ);
        if (notifier->type() == Notifier::Type::Write)
            WSAEventSelect(notifier->fd(), events[notifier->fd()], FD_WRITE);
        if (notifier->type() == Notifier::Type::Exceptional)
            TODO();
    }

    bool has_pending_events = ThreadEventQueue::current().has_pending_events();

    // Figure out how long to wait at maximum.
    // This mainly depends on the PumpMode and whether we have pending events, but also the next expiring timer.
    MonotonicTime now = MonotonicTime::now_coarse();
    struct timeval timeout = { 0, 0 };
    bool should_wait_forever = false;
    if (mode == EventLoopImplementation::PumpMode::WaitForEvents && !has_pending_events) {
        auto next_timer_expiration = get_next_timer_expiration();
        if (next_timer_expiration.has_value()) {
            now = MonotonicTime::now();
            auto computed_timeout = next_timer_expiration.value() - now;
            if (computed_timeout.is_negative())
                computed_timeout = Duration::zero();
            timeout = computed_timeout.to_timeval();
        } else {
            should_wait_forever = true;
        }
    }

    if (should_wait_forever) {
        dbgln("EventLoopManagerWindows::wait_for_events: select (max_fd={}, timeout=FOREVER)", max_socket);
    } else {
        dbgln("EventLoopManagerWindows::wait_for_events: select (max_fd={}, timeout={})", max_socket, timeout.tv_sec * 1000 + timeout.tv_usec / 1000);
    }

    DWORD rc = WSAWaitForMultipleEvents(thread_data.notifiers.size() + 1, events, FALSE, should_wait_forever ? WSA_INFINITE : timeout.tv_sec * 1000 + timeout.tv_usec / 1000, FALSE);
    for (size_t i = 0; i < thread_data.notifiers.size(); ++i) {
        if (rc == WSA_WAIT_EVENT_0 + i) {
            dbgln("WSAWaitForMultipleEvents ({}) failed with error: {}", i, WSAGetLastError());
            VERIFY_NOT_REACHED();
        }
    }

    if (!thread_data.timers.is_empty()) {
        now = MonotonicTime::now();
    }

    // Handle expired timers.
    for (auto& it : thread_data.timers) {
        auto& timer = *it.value;
        if (!timer.has_expired(now))
            continue;
        auto owner = timer.owner.strong_ref();
        if (timer.fire_when_not_visible == TimerShouldFireWhenNotVisible::No
            && owner && !owner->is_visible_for_timer_purposes()) {
            continue;
        }

        if (owner)
            ThreadEventQueue::current().post_event(*owner, make<TimerEvent>(timer.timer_id));
        if (timer.should_reload) {
            timer.reload(now);
        } else {
            // FIXME: Support removing expired timers that don't want to reload.
            VERIFY_NOT_REACHED();
        }
    }

    // Handle file system notifiers by making them normal events.
    for (auto& notifier : thread_data.notifiers) {
        if (notifier->type() == Notifier::Type::Read) {
            ThreadEventQueue::current().post_event(*notifier, make<NotifierActivationEvent>(notifier->fd()));
        }
        if (notifier->type() == Notifier::Type::Write) {
            ThreadEventQueue::current().post_event(*notifier, make<NotifierActivationEvent>(notifier->fd()));
        }
    }
}

class SignalHandlers : public RefCounted<SignalHandlers> {
    AK_MAKE_NONCOPYABLE(SignalHandlers);
    AK_MAKE_NONMOVABLE(SignalHandlers);

public:
    SignalHandlers(int signal_number, void (*handle_signal)(int));
    ~SignalHandlers();

    void dispatch();
    int add(Function<void(int)>&& handler);
    bool remove(int handler_id);

    bool is_empty() const
    {
        if (m_calling_handlers) {
            for (auto& handler : m_handlers_pending) {
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
    void (*m_original_handler)(int); // TODO: can't use sighandler_t?
    HashMap<int, Function<void(int)>> m_handlers;
    HashMap<int, Function<void(int)>> m_handlers_pending;
    bool m_calling_handlers { false };
};

struct SignalHandlersInfo {
    HashMap<int, NonnullRefPtr<SignalHandlers>> signal_handlers;
    int next_signal_id { 0 };
};

static Singleton<SignalHandlersInfo> s_signals;
template<bool create_if_null = true>
inline SignalHandlersInfo* signals_info()
{
    return s_signals.ptr();
}

void EventLoopManagerWindows::dispatch_signal(int signal_number)
{
    auto& info = *signals_info();
    auto handlers = info.signal_handlers.find(signal_number);
    if (handlers != info.signal_handlers.end()) {
        // Make sure we bump the ref count while dispatching the handlers!
        // This allows a handler to unregister/register while the handlers
        // are being called!
        auto handler = handlers->value;
        handler->dispatch();
    }
}

void EventLoopImplementationWindows::notify_forked_and_in_child()
{
    auto& thread_data = ThreadData::the();
    thread_data.timers.clear();
    thread_data.notifiers.clear();
    thread_data.initialize_wake_pipe();
    if (auto* info = signals_info<false>()) {
        info->signal_handlers.clear();
        info->next_signal_id = 0;
    }
    thread_data.pid = GetCurrentProcessId();
}

Optional<MonotonicTime> EventLoopManagerWindows::get_next_timer_expiration()
{
    auto now = MonotonicTime::now_coarse();
    Optional<MonotonicTime> soonest {};
    for (auto& it : ThreadData::the().timers) {
        auto& fire_time = it.value->fire_time;
        auto owner = it.value->owner.strong_ref();
        if (it.value->fire_when_not_visible == TimerShouldFireWhenNotVisible::No
            && owner && !owner->is_visible_for_timer_purposes()) {
            continue;
        }
        // OPTIMIZATION: If we have a timer that needs to fire right away, we can stop looking here.
        // FIXME: This whole operation could be O(1) with a better data structure.
        if (fire_time < now)
            return now;
        if (!soonest.has_value() || fire_time < soonest.value())
            soonest = fire_time;
    }
    return soonest;
}

SignalHandlers::SignalHandlers(int signal_number, void (*handle_signal)(int))
    : m_signal_number(signal_number)
    , m_original_handler(signal(signal_number, handle_signal))
{
}

SignalHandlers::~SignalHandlers()
{
    signal(m_signal_number, m_original_handler);
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

void EventLoopManagerWindows::handle_signal(int signal_number)
{
    VERIFY(signal_number != 0);
    auto& thread_data = ThreadData::the();
    // We MUST check if the current pid still matches, because there
    // is a window between fork() and exec() where a signal delivered
    // to our fork could be inadvertently routed to the parent process!
    if (GetCurrentProcessId() == thread_data.pid) {
        DWORD nwritten;
        WriteFile(thread_data.wake_pipe_read, &signal_number, sizeof(signal_number), &nwritten, nullptr);
        if (nwritten < sizeof(signal_number)) {
            perror("WriteFile");
            VERIFY_NOT_REACHED();
        }
    } else {
        // We're a fork who received a signal, reset thread_data.pid.
        thread_data.pid = GetCurrentProcessId();
    }
}

int EventLoopManagerWindows::register_signal(int signal_number, Function<void(int)> handler)
{
    VERIFY(signal_number != 0);
    auto& info = *signals_info();
    auto handlers = info.signal_handlers.find(signal_number);
    if (handlers == info.signal_handlers.end()) {
        auto signal_handlers = adopt_ref(*new SignalHandlers(signal_number, EventLoopManagerWindows::handle_signal));
        auto handler_id = signal_handlers->add(move(handler));
        info.signal_handlers.set(signal_number, move(signal_handlers));
        return handler_id;
    } else {
        return handlers->value->add(move(handler));
    }
}

void EventLoopManagerWindows::unregister_signal(int handler_id)
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

int EventLoopManagerWindows::register_timer(EventReceiver& object, int milliseconds, bool should_reload, TimerShouldFireWhenNotVisible fire_when_not_visible)
{
    VERIFY(milliseconds >= 0);
    auto& thread_data = ThreadData::the();
    auto timer = make<EventLoopTimer>();
    timer->owner = object;
    timer->interval = Duration::from_milliseconds(milliseconds);
    timer->reload(MonotonicTime::now_coarse());
    timer->should_reload = should_reload;
    timer->fire_when_not_visible = fire_when_not_visible;
    int timer_id = thread_data.id_allocator.allocate();
    timer->timer_id = timer_id;
    thread_data.timers.set(timer_id, move(timer));
    return timer_id;
}

bool EventLoopManagerWindows::unregister_timer(int timer_id)
{
    auto& thread_data = ThreadData::the();
    thread_data.id_allocator.deallocate(timer_id);
    return thread_data.timers.remove(timer_id);
}

void EventLoopManagerWindows::register_notifier(Notifier& notifier)
{
    ThreadData::the().notifiers.set(&notifier);
}

void EventLoopManagerWindows::unregister_notifier(Notifier& notifier)
{
    ThreadData::the().notifiers.remove(&notifier);
}

void EventLoopManagerWindows::did_post_event()
{
}

EventLoopManagerWindows::~EventLoopManagerWindows() = default;

NonnullOwnPtr<EventLoopImplementation> EventLoopManagerWindows::make_implementation()
{
    return adopt_own(*new EventLoopImplementationWindows);
}

}
