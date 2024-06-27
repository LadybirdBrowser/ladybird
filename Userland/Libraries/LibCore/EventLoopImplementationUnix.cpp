/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinaryHeap.h>
#include <AK/Singleton.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <AK/WeakPtr.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoopImplementationUnix.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <pthread.h>
#include <sys/select.h>
#include <unistd.h>

namespace Core {

namespace {
struct ThreadData;
class TimeoutSet;

HashMap<pthread_t, ThreadData*> s_thread_data;
pthread_key_t s_thread_key;
static pthread_rwlock_t s_thread_data_lock_impl;
static pthread_rwlock_t* s_thread_data_lock = nullptr;
thread_local pthread_t s_thread_id;
thread_local OwnPtr<ThreadData> s_this_thread_data;

short notification_type_to_poll_events(NotificationType type)
{
    short events = 0;
    if (has_flag(type, NotificationType::Read))
        events |= POLLIN;
    if (has_flag(type, NotificationType::Write))
        events |= POLLOUT;
    return events;
}

bool has_flag(int value, int flag)
{
    return (value & flag) == flag;
}

class EventLoopTimeout {
public:
    static constexpr ssize_t INVALID_INDEX = NumericLimits<ssize_t>::max();

    EventLoopTimeout() { }
    virtual ~EventLoopTimeout() = default;

    virtual void fire(TimeoutSet& timeout_set, MonotonicTime time) = 0;

    MonotonicTime fire_time() const { return m_fire_time; }

    void absolutize(Badge<TimeoutSet>, MonotonicTime current_time)
    {
        m_fire_time = current_time + m_duration;
    }

    ssize_t& index(Badge<TimeoutSet>) { return m_index; }
    void set_index(Badge<TimeoutSet>, ssize_t index) { m_index = index; }

    bool is_scheduled() const { return m_index != INVALID_INDEX; }

protected:
    union {
        Duration m_duration;
        MonotonicTime m_fire_time;
    };

private:
    ssize_t m_index = INVALID_INDEX;
};

class TimeoutSet {
public:
    TimeoutSet() = default;

    Optional<MonotonicTime> next_timer_expiration()
    {
        if (!m_heap.is_empty()) {
            return m_heap.peek_min()->fire_time();
        } else {
            return {};
        }
    }

    void absolutize_relative_timeouts(MonotonicTime current_time)
    {
        for (auto timeout : m_scheduled_timeouts) {
            timeout->absolutize({}, current_time);
            m_heap.insert(timeout);
        }
        m_scheduled_timeouts.clear();
    }

    size_t fire_expired(MonotonicTime current_time)
    {
        size_t fired_count = 0;
        while (!m_heap.is_empty()) {
            auto& timeout = *m_heap.peek_min();

            if (timeout.fire_time() <= current_time) {
                ++fired_count;
                m_heap.pop_min();
                timeout.set_index({}, EventLoopTimeout::INVALID_INDEX);
                timeout.fire(*this, current_time);
            } else {
                break;
            }
        }
        return fired_count;
    }

    void schedule_relative(EventLoopTimeout* timeout)
    {
        timeout->set_index({}, -1 - static_cast<ssize_t>(m_scheduled_timeouts.size()));
        m_scheduled_timeouts.append(timeout);
    }

    void schedule_absolute(EventLoopTimeout* timeout)
    {
        m_heap.insert(timeout);
    }

    void unschedule(EventLoopTimeout* timeout)
    {
        if (timeout->index({}) < 0) {
            size_t i = -1 - timeout->index({});
            size_t j = m_scheduled_timeouts.size() - 1;
            VERIFY(m_scheduled_timeouts[i] == timeout);
            swap(m_scheduled_timeouts[i], m_scheduled_timeouts[j]);
            swap(m_scheduled_timeouts[i]->index({}), m_scheduled_timeouts[j]->index({}));
            (void)m_scheduled_timeouts.take_last();
        } else {
            m_heap.pop(timeout->index({}));
        }
        timeout->set_index({}, EventLoopTimeout::INVALID_INDEX);
    }

    void clear()
    {
        for (auto* timeout : m_heap.nodes_in_arbitrary_order())
            timeout->set_index({}, EventLoopTimeout::INVALID_INDEX);
        m_heap.clear();
        for (auto* timeout : m_scheduled_timeouts)
            timeout->set_index({}, EventLoopTimeout::INVALID_INDEX);
        m_scheduled_timeouts.clear();
    }

private:
    IntrusiveBinaryHeap<
        EventLoopTimeout*,
        decltype([](EventLoopTimeout* a, EventLoopTimeout* b) {
            return a->fire_time() < b->fire_time();
        }),
        decltype([](EventLoopTimeout* timeout, size_t index) {
            timeout->set_index({}, static_cast<ssize_t>(index));
        }),
        8>
        m_heap;
    Vector<EventLoopTimeout*, 8> m_scheduled_timeouts;
};

class EventLoopTimer final : public EventLoopTimeout {
public:
    EventLoopTimer() = default;

    void reload(MonotonicTime const& now) { m_fire_time = now + interval; }

    virtual void fire(TimeoutSet& timeout_set, MonotonicTime current_time) override
    {
        auto strong_owner = owner.strong_ref();

        if (!strong_owner)
            return;

        if (should_reload) {
            MonotonicTime next_fire_time = m_fire_time + interval;
            if (next_fire_time <= current_time) {
                next_fire_time = current_time + interval;
            }
            m_fire_time = next_fire_time;
            if (next_fire_time != current_time) {
                timeout_set.schedule_absolute(this);
            } else {
                // NOTE: Unfortunately we need to treat timeouts with the zero interval in a
                //       special way. TimeoutSet::schedule_absolute for them will result in an
                //       infinite loop. TimeoutSet::schedule_relative, on the other hand, will do a
                //       correct thing of scheduling them for the next iteration of the loop.
                m_duration = {};
                timeout_set.schedule_relative(this);
            }
        }

        // FIXME: While TimerShouldFireWhenNotVisible::Yes prevents the timer callback from being
        //        called, it doesn't allow event loop to sleep since it needs to constantly check if
        //        is_visible_for_timer_purposes changed. A better solution will be to unregister a
        //        timer and register it back again when needed. This also has an added benefit of
        //        making fire_when_not_visible and is_visible_for_timer_purposes obsolete.
        if (fire_when_not_visible == TimerShouldFireWhenNotVisible::Yes || strong_owner->is_visible_for_timer_purposes())
            ThreadEventQueue::current().post_event(*strong_owner, make<TimerEvent>());
    }

    Duration interval;
    bool should_reload { false };
    TimerShouldFireWhenNotVisible fire_when_not_visible { TimerShouldFireWhenNotVisible::No };
    WeakPtr<EventReceiver> owner;
    pthread_t owner_thread { 0 };
    Atomic<bool> is_being_deleted { false };
};

struct ThreadData {
    static ThreadData& the()
    {
        if (!s_thread_data_lock) {
            pthread_rwlock_init(&s_thread_data_lock_impl, nullptr);
            s_thread_data_lock = &s_thread_data_lock_impl;
            pthread_key_create(&s_thread_key, [](void*) {
                s_this_thread_data.clear();
            });
        }

        if (s_thread_id == 0)
            s_thread_id = pthread_self();
        ThreadData* data = nullptr;
        if (!s_this_thread_data) {
            data = new ThreadData;
            s_this_thread_data = adopt_own(*data);

            pthread_rwlock_wrlock(&*s_thread_data_lock);
            s_thread_data.set(s_thread_id, s_this_thread_data.ptr());
            pthread_rwlock_unlock(&*s_thread_data_lock);
        } else {
            data = s_this_thread_data.ptr();
        }
        return *data;
    }

    static ThreadData* for_thread(pthread_t thread_id)
    {
        pthread_rwlock_rdlock(&*s_thread_data_lock);
        auto result = s_thread_data.get(thread_id).value_or(nullptr);
        pthread_rwlock_unlock(&*s_thread_data_lock);
        return result;
    }

    ThreadData()
    {
        pid = getpid();
        initialize_wake_pipe();
    }

    ~ThreadData()
    {
        pthread_rwlock_wrlock(&*s_thread_data_lock);
        s_thread_data.remove(s_thread_id);
        pthread_rwlock_unlock(&*s_thread_data_lock);
    }

    void initialize_wake_pipe()
    {
        if (wake_pipe_fds[0] != -1)
            close(wake_pipe_fds[0]);
        if (wake_pipe_fds[1] != -1)
            close(wake_pipe_fds[1]);

        wake_pipe_fds = MUST(Core::System::pipe2(O_CLOEXEC));

        // The wake pipe informs us of POSIX signals as well as manual calls to wake()
        VERIFY(poll_fds.size() == 0);
        poll_fds.append({ .fd = wake_pipe_fds[0], .events = POLLIN, .revents = 0 });
        notifier_by_index.append(nullptr);
    }

    // Each thread has its own timers, notifiers and a wake pipe.
    TimeoutSet timeouts;

    Vector<pollfd> poll_fds;
    HashMap<Notifier*, size_t> notifier_by_ptr;
    Vector<Notifier*> notifier_by_index;

    // The wake pipe is used to notify another event loop that someone has called wake(), or a signal has been received.
    // wake() writes 0i32 into the pipe, signals write the signal number (guaranteed non-zero).
    Array<int, 2> wake_pipe_fds { -1, -1 };

    pid_t pid { 0 };
};
}

EventLoopImplementationUnix::EventLoopImplementationUnix()
    : m_wake_pipe_fds(ThreadData::the().wake_pipe_fds)
{
}

EventLoopImplementationUnix::~EventLoopImplementationUnix() = default;

int EventLoopImplementationUnix::exec()
{
    for (;;) {
        if (m_exit_requested)
            return m_exit_code;
        pump(PumpMode::WaitForEvents);
    }
    VERIFY_NOT_REACHED();
}

size_t EventLoopImplementationUnix::pump(PumpMode mode)
{
    static_cast<EventLoopManagerUnix&>(EventLoopManager::the()).wait_for_events(mode);
    return ThreadEventQueue::current().process();
}

void EventLoopImplementationUnix::quit(int code)
{
    m_exit_requested = true;
    m_exit_code = code;
}

void EventLoopImplementationUnix::unquit()
{
    m_exit_requested = false;
    m_exit_code = 0;
}

bool EventLoopImplementationUnix::was_exit_requested() const
{
    return m_exit_requested;
}

void EventLoopImplementationUnix::post_event(EventReceiver& receiver, NonnullOwnPtr<Event>&& event)
{
    m_thread_event_queue.post_event(receiver, move(event));
    if (&m_thread_event_queue != &ThreadEventQueue::current())
        wake();
}

void EventLoopImplementationUnix::wake()
{
    int wake_event = 0;
    MUST(Core::System::write(m_wake_pipe_fds[1], { &wake_event, sizeof(wake_event) }));
}

void EventLoopManagerUnix::wait_for_events(EventLoopImplementation::PumpMode mode)
{
    auto& thread_data = ThreadData::the();

retry:
    bool has_pending_events = ThreadEventQueue::current().has_pending_events();

    auto time_at_iteration_start = MonotonicTime::now_coarse();
    thread_data.timeouts.absolutize_relative_timeouts(time_at_iteration_start);

    // Figure out how long to wait at maximum.
    // This mainly depends on the PumpMode and whether we have pending events, but also the next expiring timer.
    int timeout = 0;
    bool should_wait_forever = false;
    if (mode == EventLoopImplementation::PumpMode::WaitForEvents && !has_pending_events) {
        auto next_timer_expiration = thread_data.timeouts.next_timer_expiration();
        if (next_timer_expiration.has_value()) {
            auto computed_timeout = next_timer_expiration.value() - time_at_iteration_start;
            if (computed_timeout.is_negative())
                computed_timeout = Duration::zero();
            i64 true_timeout = computed_timeout.to_milliseconds();
            timeout = static_cast<i32>(min<i64>(AK::NumericLimits<i32>::max(), true_timeout));
        } else {
            should_wait_forever = true;
        }
    }

try_select_again:
    // select() and wait for file system events, calls to wake(), POSIX signals, or timer expirations.
    ErrorOr<int> error_or_marked_fd_count = System::poll(thread_data.poll_fds, should_wait_forever ? -1 : timeout);
    auto time_after_poll = MonotonicTime::now_coarse();
    // Because POSIX, we might spuriously return from select() with EINTR; just select again.
    if (error_or_marked_fd_count.is_error()) {
        if (error_or_marked_fd_count.error().code() == EINTR)
            goto try_select_again;
        dbgln("EventLoopImplementationUnix::wait_for_events: {}", error_or_marked_fd_count.error());
        VERIFY_NOT_REACHED();
    }

    // We woke up due to a call to wake() or a POSIX signal.
    // Handle signals and see whether we need to handle events as well.
    if (has_flag(thread_data.poll_fds[0].revents, POLLIN)) {
        int wake_events[8];
        ssize_t nread;
        // We might receive another signal while read()ing here. The signal will go to the handle_signal properly,
        // but we get interrupted. Therefore, just retry while we were interrupted.
        do {
            errno = 0;
            nread = read(thread_data.wake_pipe_fds[0], wake_events, sizeof(wake_events));
            if (nread == 0)
                break;
        } while (nread < 0 && errno == EINTR);
        if (nread < 0) {
            perror("EventLoopImplementationUnix::wait_for_events: read from wake pipe");
            VERIFY_NOT_REACHED();
        }
        VERIFY(nread > 0);
        bool wake_requested = false;
        int event_count = nread / sizeof(wake_events[0]);
        for (int i = 0; i < event_count; i++) {
            if (wake_events[i] != 0)
                dispatch_signal(wake_events[i]);
            else
                wake_requested = true;
        }

        if (!wake_requested && nread == sizeof(wake_events))
            goto retry;
    }

    if (error_or_marked_fd_count.value() != 0) {
        // Handle file system notifiers by making them normal events.
        for (size_t i = 1; i < thread_data.poll_fds.size(); ++i) {
            auto& revents = thread_data.poll_fds[i].revents;
            auto& notifier = *thread_data.notifier_by_index[i];

            NotificationType type = NotificationType::None;
            if (has_flag(revents, POLLIN))
                type |= NotificationType::Read;
            if (has_flag(revents, POLLOUT))
                type |= NotificationType::Write;
            if (has_flag(revents, POLLHUP))
                type |= NotificationType::HangUp;
            if (has_flag(revents, POLLERR))
                type |= NotificationType::Error;
            type &= notifier.type();
            if (type != NotificationType::None)
                ThreadEventQueue::current().post_event(notifier, make<NotifierActivationEvent>(notifier.fd(), type));
        }
    }

    // Handle expired timers.
    thread_data.timeouts.fire_expired(time_after_poll);
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

void EventLoopManagerUnix::dispatch_signal(int signal_number)
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

void EventLoopImplementationUnix::notify_forked_and_in_child()
{
    auto& thread_data = ThreadData::the();
    thread_data.timeouts.clear();
    thread_data.poll_fds.clear();
    thread_data.notifier_by_ptr.clear();
    thread_data.notifier_by_index.clear();
    thread_data.initialize_wake_pipe();
    if (auto* info = signals_info<false>()) {
        info->signal_handlers.clear();
        info->next_signal_id = 0;
    }
    thread_data.pid = getpid();
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

void EventLoopManagerUnix::handle_signal(int signal_number)
{
    VERIFY(signal_number != 0);
    auto& thread_data = ThreadData::the();
    // We MUST check if the current pid still matches, because there
    // is a window between fork() and exec() where a signal delivered
    // to our fork could be inadvertently routed to the parent process!
    if (getpid() == thread_data.pid) {
        int nwritten = write(thread_data.wake_pipe_fds[1], &signal_number, sizeof(signal_number));
        if (nwritten < 0) {
            perror("EventLoopImplementationUnix::register_signal: write");
            VERIFY_NOT_REACHED();
        }
    } else {
        // We're a fork who received a signal, reset thread_data.pid.
        thread_data.pid = getpid();
    }
}

int EventLoopManagerUnix::register_signal(int signal_number, Function<void(int)> handler)
{
    VERIFY(signal_number != 0);
    auto& info = *signals_info();
    auto handlers = info.signal_handlers.find(signal_number);
    if (handlers == info.signal_handlers.end()) {
        auto signal_handlers = adopt_ref(*new SignalHandlers(signal_number, EventLoopManagerUnix::handle_signal));
        auto handler_id = signal_handlers->add(move(handler));
        info.signal_handlers.set(signal_number, move(signal_handlers));
        return handler_id;
    } else {
        return handlers->value->add(move(handler));
    }
}

void EventLoopManagerUnix::unregister_signal(int handler_id)
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

intptr_t EventLoopManagerUnix::register_timer(EventReceiver& object, int milliseconds, bool should_reload, TimerShouldFireWhenNotVisible fire_when_not_visible)
{
    VERIFY(milliseconds >= 0);
    auto& thread_data = ThreadData::the();
    auto timer = new EventLoopTimer;
    timer->owner_thread = s_thread_id;
    timer->owner = object;
    timer->interval = Duration::from_milliseconds(milliseconds);
    timer->reload(MonotonicTime::now_coarse());
    timer->should_reload = should_reload;
    timer->fire_when_not_visible = fire_when_not_visible;
    thread_data.timeouts.schedule_absolute(timer);
    return bit_cast<intptr_t>(timer);
}

void EventLoopManagerUnix::unregister_timer(intptr_t timer_id)
{
    auto* timer = bit_cast<EventLoopTimer*>(timer_id);
    auto thread_data_ptr = ThreadData::for_thread(timer->owner_thread);
    if (!thread_data_ptr)
        return;
    auto& thread_data = *thread_data_ptr;
    auto expected = false;
    if (timer->is_being_deleted.compare_exchange_strong(expected, true, AK::MemoryOrder::memory_order_acq_rel)) {
        if (timer->is_scheduled())
            thread_data.timeouts.unschedule(timer);
        delete timer;
    }
}

void EventLoopManagerUnix::register_notifier(Notifier& notifier)
{
    auto& thread_data = ThreadData::the();

    thread_data.notifier_by_ptr.set(&notifier, thread_data.poll_fds.size());
    thread_data.notifier_by_index.append(&notifier);
    thread_data.poll_fds.append({
        .fd = notifier.fd(),
        .events = notification_type_to_poll_events(notifier.type()),
        .revents = 0,
    });

    notifier.set_owner_thread(s_thread_id);
}

void EventLoopManagerUnix::unregister_notifier(Notifier& notifier)
{
    auto thread_data_ptr = ThreadData::for_thread(notifier.owner_thread());
    if (!thread_data_ptr)
        return;

    auto& thread_data = *thread_data_ptr;
    auto it = thread_data.notifier_by_ptr.find(&notifier);
    VERIFY(it != thread_data.notifier_by_ptr.end());

    size_t notifier_index = it->value;
    thread_data.notifier_by_ptr.remove(it);

    if (notifier_index + 1 != thread_data.poll_fds.size()) {
        swap(thread_data.poll_fds[notifier_index], thread_data.poll_fds.last());
        swap(thread_data.notifier_by_index[notifier_index], thread_data.notifier_by_index.last());
        thread_data.notifier_by_ptr.set(thread_data.notifier_by_index[notifier_index], notifier_index);
    }
    thread_data.poll_fds.take_last();
    thread_data.notifier_by_index.take_last();
}

void EventLoopManagerUnix::did_post_event()
{
}

EventLoopManagerUnix::~EventLoopManagerUnix() = default;

NonnullOwnPtr<EventLoopImplementation> EventLoopManagerUnix::make_implementation()
{
    return adopt_own(*new EventLoopImplementationUnix);
}

}
