/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibCore/Event.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibThreading/Mutex.h>
#include <UI/Gtk/EventLoopImplementationGtk.h>

#include <glib-unix.h>
#include <glib.h>

namespace Ladybird {

static HashMap<Core::Notifier*, guint> s_notifiers;
static Threading::Mutex s_notifiers_mutex;

// Signal handling for signals not supported by g_unix_signal_add
// (which only handles SIGHUP, SIGINT, SIGTERM, SIGUSR1, SIGUSR2, SIGWINCH).
// For unsupported signals like SIGCHLD, we use a pipe-based approach.

static int s_signal_pipe_fds[2] = { -1, -1 };

static HashMap<int, Function<void(int)>> s_pipe_signal_handlers;

static void pipe_signal_handler(int signal_number)
{
    [[maybe_unused]] auto _ = ::write(s_signal_pipe_fds[1], &signal_number, sizeof(signal_number));
}

static gboolean pipe_signal_callback(gint fd, [[maybe_unused]] GIOCondition condition, [[maybe_unused]] gpointer data)
{
    int signal_number = {};
    ssize_t nread;
    do {
        errno = 0;
        nread = read(fd, &signal_number, sizeof(signal_number));
        if (nread >= 0)
            break;
    } while (errno == EINTR);
    if (nread == sizeof(signal_number)) {
        auto it = s_pipe_signal_handlers.find(signal_number);
        if (it != s_pipe_signal_handlers.end())
            it->value(signal_number);
    }
    return G_SOURCE_CONTINUE;
}

static void ensure_signal_pipe()
{
    if (s_signal_pipe_fds[0] != -1)
        return;
    auto fds = MUST(Core::System::pipe2(O_CLOEXEC));
    s_signal_pipe_fds[0] = fds[0];
    s_signal_pipe_fds[1] = fds[1];
    g_unix_fd_add(s_signal_pipe_fds[0], G_IO_IN, pipe_signal_callback, nullptr);
}

static bool glib_supports_signal(int signum)
{
    return signum == SIGHUP || signum == SIGINT || signum == SIGTERM
        || signum == SIGUSR1 || signum == SIGUSR2 || signum == SIGWINCH;
}

// Timer callback

struct TimerData {
    WeakPtr<Core::EventReceiver> weak_object;
    bool should_reload;
};

static gboolean timer_callback(gpointer user_data)
{
    auto* data = static_cast<TimerData*>(user_data);
    auto object = data->weak_object.strong_ref();
    if (!object)
        return G_SOURCE_REMOVE;
    Core::TimerEvent event;
    object->dispatch_event(event);
    return data->should_reload ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void timer_destroy(gpointer user_data)
{
    delete static_cast<TimerData*>(user_data);
}

// Notifier callback

static gboolean notifier_callback([[maybe_unused]] gint fd, [[maybe_unused]] GIOCondition condition, gpointer user_data)
{
    auto notifier = static_cast<WeakPtr<Core::EventReceiver>*>(user_data)->strong_ref();
    if (!notifier)
        return G_SOURCE_REMOVE;
    Core::NotifierActivationEvent event;
    notifier->dispatch_event(event);
    return G_SOURCE_CONTINUE;
}

static void notifier_destroy(gpointer user_data)
{
    delete static_cast<WeakPtr<Core::EventReceiver>*>(user_data);
}

// EventLoopManagerGtk

NonnullOwnPtr<Core::EventLoopImplementation> EventLoopManagerGtk::make_implementation()
{
    return EventLoopImplementationGtk::create();
}

intptr_t EventLoopManagerGtk::register_timer(Core::EventReceiver& object, int milliseconds, bool should_reload)
{
    auto* data = new TimerData { object.make_weak_ptr(), should_reload };
    auto source_id = g_timeout_add_full(G_PRIORITY_DEFAULT, milliseconds, timer_callback, data, timer_destroy);
    return static_cast<intptr_t>(source_id);
}

void EventLoopManagerGtk::unregister_timer(intptr_t timer_id)
{
    g_source_remove(static_cast<guint>(timer_id));
}

void EventLoopManagerGtk::register_notifier(Core::Notifier& notifier)
{
    GIOCondition condition {};
    switch (notifier.type()) {
    case Core::Notifier::Type::Read:
        condition = G_IO_IN;
        break;
    case Core::Notifier::Type::Write:
        condition = G_IO_OUT;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    auto weak_notifier = new WeakPtr<Core::EventReceiver>(notifier.make_weak_ptr());
    auto source_id = g_unix_fd_add_full(G_PRIORITY_DEFAULT, notifier.fd(), condition, notifier_callback, weak_notifier, notifier_destroy);

    Threading::MutexLocker locker(s_notifiers_mutex);
    s_notifiers.set(&notifier, source_id);
}

void EventLoopManagerGtk::unregister_notifier(Core::Notifier& notifier)
{
    Threading::MutexLocker locker(s_notifiers_mutex);
    auto it = s_notifiers.find(&notifier);
    if (it == s_notifiers.end())
        return;
    g_source_remove(it->value);
    s_notifiers.remove(it);
}

void EventLoopManagerGtk::did_post_event()
{
    if (m_idle_pending)
        return;
    m_idle_pending = true;
    g_idle_add_once(
        [](gpointer data) {
            auto& self = *static_cast<EventLoopManagerGtk*>(data);
            self.m_idle_pending = false;
            Core::ThreadEventQueue::current().process();
        },
        this);
}

int EventLoopManagerGtk::register_signal(int signal_number, Function<void(int)> handler)
{
    VERIFY(signal_number != 0);

    if (glib_supports_signal(signal_number)) {
        struct Data {
            int signal_number;
            Function<void(int)> handler;
        };
        auto* data = new Data { signal_number, move(handler) };
        return static_cast<int>(g_unix_signal_add_full(
            G_PRIORITY_DEFAULT,
            signal_number,
            [](gpointer user_data) -> gboolean {
                auto* data = static_cast<Data*>(user_data);
                data->handler(data->signal_number);
                return G_SOURCE_CONTINUE;
            },
            data,
            [](gpointer user_data) { delete static_cast<Data*>(user_data); }));
    }

    // For signals GLib doesn't support (e.g. SIGCHLD), use a pipe-based approach.
    ensure_signal_pipe();
    s_pipe_signal_handlers.set(signal_number, move(handler));
    ::signal(signal_number, pipe_signal_handler);
    // Return negative IDs for pipe-based handlers to distinguish from GSource IDs.
    return -signal_number;
}

void EventLoopManagerGtk::unregister_signal(int handler_id)
{
    VERIFY(handler_id != 0);

    if (handler_id > 0) {
        g_source_remove(static_cast<guint>(handler_id));
        return;
    }

    // Negative handler_id means pipe-based handler; the signal number is -handler_id.
    int signal_number = -handler_id;
    ::signal(signal_number, SIG_DFL);
    s_pipe_signal_handlers.remove(signal_number);
}

// EventLoopImplementationGtk

EventLoopImplementationGtk::EventLoopImplementationGtk()
    : m_loop(g_main_loop_new(nullptr, FALSE))
{
}

EventLoopImplementationGtk::~EventLoopImplementationGtk()
{
    g_clear_pointer(&m_loop, g_main_loop_unref);
}

int EventLoopImplementationGtk::exec()
{
    g_main_loop_run(m_loop);
    return m_exit_code;
}

size_t EventLoopImplementationGtk::pump(PumpMode mode)
{
    auto result = Core::ThreadEventQueue::current().process();
    auto may_block = (mode == PumpMode::WaitForEvents) ? TRUE : FALSE;
    g_main_context_iteration(g_main_loop_get_context(m_loop), may_block);
    result += Core::ThreadEventQueue::current().process();
    return result;
}

void EventLoopImplementationGtk::quit(int code)
{
    m_exit_code = code;
    m_exit_requested = true;
    if (m_loop && g_main_loop_is_running(m_loop))
        g_main_loop_quit(m_loop);
}

void EventLoopImplementationGtk::wake()
{
    g_main_context_wakeup(g_main_loop_get_context(m_loop));
}

bool EventLoopImplementationGtk::was_exit_requested() const
{
    return m_exit_requested;
}

void EventLoopImplementationGtk::set_main_loop()
{
}

}
