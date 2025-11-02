/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/IDAllocator.h>
#include <AK/Singleton.h>
#include <AK/TemporaryChange.h>
#include <LibCore/Event.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/RWLock.h>
#include <LibWebView/EventLoop/EventLoopImplementationQt.h>
#include <LibWebView/EventLoop/EventLoopImplementationQtEventTarget.h>

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>

namespace WebView {

struct ThreadData;
static thread_local OwnPtr<ThreadData> s_this_thread_data;
#if defined(AK_OS_WINDOWS)
#    define THREAD_ID_KEY intptr_t
#else
#    define THREAD_ID_KEY pthread_t
#endif
static HashMap<THREAD_ID_KEY, ThreadData*> s_thread_data;
static Threading::RWLock s_thread_data_lock;
static thread_local pthread_t s_thread_id;

static THREAD_ID_KEY thread_id_key(pthread_t thread_id)
{
#if defined(AK_OS_WINDOWS)
    return reinterpret_cast<intptr_t>(thread_id.p);
#else
    return thread_id;
#endif
}

struct ThreadData {
    static ThreadData& the()
    {
        if (thread_id_key(s_thread_id) == 0)
            s_thread_id = pthread_self();
        if (!s_this_thread_data) {
            s_this_thread_data = make<ThreadData>();
            Threading::RWLockLocker<Threading::LockMode::Write> locker(s_thread_data_lock);
            s_thread_data.set(thread_id_key(s_thread_id), s_this_thread_data.ptr());
        }
        return *s_this_thread_data;
    }

    static ThreadData* for_thread(pthread_t thread_id)
    {
        Threading::RWLockLocker<Threading::LockMode::Read> locker(s_thread_data_lock);
        return s_thread_data.get(thread_id_key(thread_id)).value_or(nullptr);
    }

    ~ThreadData()
    {
        Threading::RWLockLocker<Threading::LockMode::Write> locker(s_thread_data_lock);
        s_thread_data.remove(thread_id_key(s_thread_id));
    }

    Threading::Mutex mutex;
    HashMap<Core::Notifier*, NonnullOwnPtr<QSocketNotifier>> notifiers;
};

class QtEventLoopManagerEvent final : public QEvent {
public:
    static QEvent::Type process_event_queue_event_type()
    {
        static auto const type = static_cast<QEvent::Type>(QEvent::registerEventType());
        return type;
    }

    QtEventLoopManagerEvent(QEvent::Type type)
        : QEvent(type)
    {
    }
};

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
};

SignalHandlers::SignalHandlers(int signal_number, void (*handle_signal)(int))
    : m_signal_number(signal_number)
    , m_original_handler(signal(signal_number, handle_signal))
{
}

SignalHandlers::~SignalHandlers()
{
    (void)::signal(m_signal_number, m_original_handler);
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

static void dispatch_signal(int signal_number)
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

EventLoopImplementationQt::EventLoopImplementationQt()
    : m_event_loop(make<QEventLoop>())
{
}

EventLoopImplementationQt::~EventLoopImplementationQt() = default;

int EventLoopImplementationQt::exec()
{
    if (is_main_loop())
        return QCoreApplication::exec();
    return m_event_loop->exec();
}

size_t EventLoopImplementationQt::pump(PumpMode mode)
{
    auto result = Core::ThreadEventQueue::current().process();
    auto qt_mode = mode == PumpMode::WaitForEvents ? QEventLoop::WaitForMoreEvents : QEventLoop::AllEvents;
    if (is_main_loop())
        QCoreApplication::processEvents(qt_mode);
    else
        m_event_loop->processEvents(qt_mode);
    result += Core::ThreadEventQueue::current().process();
    return result;
}

void EventLoopImplementationQt::quit(int code)
{
    if (is_main_loop())
        QCoreApplication::exit(code);
    else
        m_event_loop->exit(code);
}

void EventLoopImplementationQt::wake()
{
    if (!is_main_loop())
        m_event_loop->wakeUp();
}

bool EventLoopImplementationQt::was_exit_requested() const
{
    if (is_main_loop())
        return QCoreApplication::closingDown();
    return !m_event_loop->isRunning();
}

void EventLoopImplementationQt::post_event(Core::EventReceiver& receiver, NonnullOwnPtr<Core::Event>&& event)
{
    m_thread_event_queue.post_event(receiver, move(event));
    if (&m_thread_event_queue != &Core::ThreadEventQueue::current())
        wake();
}

void EventLoopImplementationQt::set_main_loop()
{
    m_main_loop = true;

    auto& event_loop_manager = static_cast<EventLoopManagerQt&>(Core::EventLoopManager::the());
    event_loop_manager.set_main_loop_signal_notifiers({});
}

static void qt_timer_fired(Core::EventReceiver& object)
{
    Core::TimerEvent event;
    object.dispatch_event(event);
}

intptr_t EventLoopManagerQt::register_timer(Core::EventReceiver& object, int milliseconds, bool should_reload)
{
    auto timer = new QTimer;
    timer->setTimerType(Qt::PreciseTimer);
    timer->setInterval(milliseconds);
    timer->setSingleShot(!should_reload);
    auto weak_object = object.make_weak_ptr();
    QObject::connect(timer, &QTimer::timeout, [weak_object = move(weak_object)] {
        auto object = weak_object.strong_ref();
        if (!object)
            return;
        qt_timer_fired(*object);
    });
    timer->start();
    return bit_cast<intptr_t>(timer);
}

void EventLoopManagerQt::unregister_timer(intptr_t timer_id)
{
    auto* timer = bit_cast<QTimer*>(timer_id);
    delete timer;
}

static void qt_notifier_activated(Core::Notifier& notifier)
{
    Core::NotifierActivationEvent event(notifier.fd(), notifier.type());
    notifier.dispatch_event(event);
}

void EventLoopManagerQt::register_notifier(Core::Notifier& notifier)
{
    QSocketNotifier::Type type;
    switch (notifier.type()) {
    case Core::Notifier::Type::Read:
        type = QSocketNotifier::Read;
        break;
    case Core::Notifier::Type::Write:
        type = QSocketNotifier::Write;
        break;
    default:
        TODO();
    }
    auto socket_notifier = make<QSocketNotifier>(notifier.fd(), type);
    QObject::connect(socket_notifier, &QSocketNotifier::activated, [weak_notifier = notifier.make_weak_ptr()] {
        if (!weak_notifier)
            return;
        qt_notifier_activated(static_cast<Core::Notifier&>(*weak_notifier));
    });

    {
        auto& thread_data = ThreadData::the();
        Threading::MutexLocker locker(thread_data.mutex);
        thread_data.notifiers.set(&notifier, move(socket_notifier));
    }
    notifier.set_owner_thread(s_thread_id);
}

void EventLoopManagerQt::unregister_notifier(Core::Notifier& notifier)
{
    auto* thread_data = ThreadData::for_thread(notifier.owner_thread());
    if (!thread_data)
        return;
    Threading::MutexLocker locker(thread_data->mutex);
    auto deleted_notifier = thread_data->notifiers.take(&notifier).release_value();
    if (QThread::currentThread() != deleted_notifier->thread()) {
        auto* deleted_notifier_ptr = deleted_notifier.ptr();
        QMetaObject::invokeMethod(deleted_notifier_ptr, [deleted_notifier = move(deleted_notifier)] { }, Qt::QueuedConnection);
    }
}

void EventLoopManagerQt::handle_signal(int signal_number)
{
    auto& that = static_cast<EventLoopManagerQt&>(Core::EventLoopManager::the());
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66425
    // Apparently warn_unused_result ignoring (void) casts is a feature
    [[maybe_unused]] auto _ = ::write(that.m_signal_socket_fds[1], &signal_number, sizeof(signal_number));
}

int EventLoopManagerQt::register_signal(int signal_number, Function<void(int)> handler)
{
    VERIFY(signal_number != 0);
    auto& info = *signals_info();
    auto handlers = info.signal_handlers.find(signal_number);
    if (handlers == info.signal_handlers.end()) {
        auto signal_handlers = adopt_ref(*new SignalHandlers(signal_number, EventLoopManagerQt::handle_signal));
        auto handler_id = signal_handlers->add(move(handler));
        info.signal_handlers.set(signal_number, move(signal_handlers));
        return handler_id;
    } else {
        return handlers->value->add(move(handler));
    }
}

void EventLoopManagerQt::unregister_signal(int handler_id)
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

void EventLoopManagerQt::did_post_event()
{
    QCoreApplication::postEvent(m_main_thread_event_target.ptr(), new QtEventLoopManagerEvent(QtEventLoopManagerEvent::process_event_queue_event_type()));
}

bool EventLoopManagerQt::event_target_received_event(Badge<EventLoopImplementationQtEventTarget>, QEvent* event)
{
    if (event->type() == QtEventLoopManagerEvent::process_event_queue_event_type()) {
        Core::ThreadEventQueue::current().process();
        return true;
    }
    return false;
}

EventLoopManagerQt::EventLoopManagerQt()
    : m_main_thread_event_target(make<EventLoopImplementationQtEventTarget>())
{
}

void EventLoopManagerQt::set_main_loop_signal_notifiers(Badge<EventLoopImplementationQt>)
{
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, m_signal_socket_fds));
    m_signal_socket_notifier = new QSocketNotifier(m_signal_socket_fds[0], QSocketNotifier::Read);
    QObject::connect(m_signal_socket_notifier, &QSocketNotifier::activated, [this] {
        int signal_number = {};
        ssize_t nread;
        do {
            errno = 0;
            nread = read(this->m_signal_socket_fds[0], &signal_number, sizeof(signal_number));
            if (nread >= 0)
                break;
        } while (errno == EINTR);
        VERIFY(nread == sizeof(signal_number));
        dispatch_signal(signal_number);
    });
    m_signal_socket_notifier->setEnabled(true);
}

EventLoopManagerQt::~EventLoopManagerQt()
{
    delete m_signal_socket_notifier;
    ::close(m_signal_socket_fds[0]);
    ::close(m_signal_socket_fds[1]);
}

NonnullOwnPtr<Core::EventLoopImplementation> EventLoopManagerQt::make_implementation()
{
    return adopt_own(*new EventLoopImplementationQt);
}

}
