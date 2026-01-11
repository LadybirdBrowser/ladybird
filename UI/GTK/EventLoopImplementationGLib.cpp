/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/IDAllocator.h>
#include <AK/TemporaryChange.h>
#include <LibCore/Event.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibCore/ThreadEventQueue.h>
#include <UI/GTK/EventLoopImplementationGLib.h>

#include <glib-unix.h>
#include <glibmm/main.h>

namespace Ladybird {

class ThreadData {
    AK_MAKE_NONCOPYABLE(ThreadData);
    AK_MAKE_NONMOVABLE(ThreadData);

    ThreadData() = default;
    ~ThreadData() = default;

public:
    static ThreadData& the()
    {
        static thread_local ThreadData s_thread_data;
        return s_thread_data;
    }

    IDAllocator timer_id_allocator;
    HashMap<int, sigc::scoped_connection> timers;
    HashMap<Core::Notifier*, sigc::scoped_connection> notifiers;
};

class CoreEventSource final : public Glib::Source {
public:
    static Glib::RefPtr<CoreEventSource> create()
    {
        return Glib::make_refptr_for_instance<CoreEventSource>(new CoreEventSource());
    }

private:
    virtual bool prepare(int& timeout) override
    {
        timeout = -1;
        return false;
    }

    virtual bool check() override
    {
        return Core::ThreadEventQueue::current().has_pending_events();
    }

    virtual bool dispatch(sigc::slot_base* slot) override
    {
        return (*static_cast<sigc::slot<bool()>*>(slot))();
    }

    CoreEventSource()
    {
        set_priority(Glib::PRIORITY_DEFAULT);
        set_can_recurse(true);
        connect_generic(sigc::slot<bool()>([]() -> bool {
            (void)Core::ThreadEventQueue::current().process();
            return true;
        }));

        g_source_set_name(gobj(), "Core::ThreadEventQueue");
    }
};

NonnullOwnPtr<Core::EventLoopImplementation> EventLoopManagerGLib::make_implementation()
{
    return EventLoopImplementationGLib::create();
}

intptr_t EventLoopManagerGLib::register_timer(Core::EventReceiver& object, int interval_milliseconds, bool should_reload)
{
    auto weak_object = object.make_weak_ptr();
    sigc::scoped_connection timer_handle = Glib::signal_timeout().connect([weak_object = move(weak_object), should_reload]() -> bool {
        auto object = weak_object.strong_ref();
        if (!object)
            return false;
        Core::TimerEvent event;
        object->dispatch_event(event);
        return should_reload;
    },
        interval_milliseconds);

    auto& thread_data = ThreadData::the();

    auto timer_id = thread_data.timer_id_allocator.allocate();

    thread_data.timers.set(timer_id, move(timer_handle));
    return timer_id;
}

void EventLoopManagerGLib::unregister_timer(intptr_t timer_id)
{
    auto& thread_data = ThreadData::the();
    auto maybe_handle = thread_data.timers.take(static_cast<int>(timer_id));
    VERIFY(maybe_handle.has_value());

    auto timer_handle = maybe_handle.release_value();
    (void)timer_handle; // drop on the floor, the scoped connection will disconnect itself
}

void EventLoopManagerGLib::register_notifier(Core::Notifier& notifier)
{
    auto condition = Glib::IOCondition {};
    auto type = notifier.type();
    if (has_flag(type, Core::Notifier::Type::Read))
        condition |= Glib::IOCondition::IO_IN;
    if (has_flag(type, Core::Notifier::Type::Write))
        condition |= Glib::IOCondition::IO_OUT;
    if (has_flag(type, Core::Notifier::Type::Error))
        condition |= Glib::IOCondition::IO_ERR;
    if (has_flag(type, Core::Notifier::Type::HangUp))
        condition |= Glib::IOCondition::IO_HUP;

    auto weak_notifier = notifier.make_weak_ptr<Core::Notifier>();
    sigc::scoped_connection connection = Glib::signal_io().connect([weak_notifier = move(weak_notifier)](Glib::IOCondition) -> bool {
        auto notifier = weak_notifier.strong_ref();
        if (!notifier)
            return false;
        Core::NotifierActivationEvent event;
        notifier->dispatch_event(event);
        return true;
    },
        notifier.fd(), condition);

    (void)ThreadData::the().notifiers.set(&notifier, move(connection));
}

void EventLoopManagerGLib::unregister_notifier(Core::Notifier& notifier)
{
    (void)ThreadData::the().notifiers.remove(&notifier);
}

static GMainContext* get_main_context()
{
    // Use the thread default context if it exists, otherwise use the default context.
    return g_main_context_get_thread_default() ?: g_main_context_default();
}

void EventLoopManagerGLib::did_post_event()
{
    g_main_context_wakeup(get_main_context());
}

int EventLoopManagerGLib::register_signal(int signum, Function<void(int)> handler)
{
    struct Closure {
        int signum;
        Function<void(int)> handler;
    };
    auto* closure = new Closure { signum, move(handler) };

    auto* source = g_unix_signal_source_new(signum);
    g_source_set_callback(source, +[](void* data) -> gboolean {
        auto* c = static_cast<Closure*>(data);
        c->handler(c->signum);
        return G_SOURCE_CONTINUE; }, closure, +[](void* data) {
        auto* closure = static_cast<Closure*>(data);
        delete closure; });

    auto id = g_source_attach(source, get_main_context());
    g_source_unref(source);
    return static_cast<int>(id);
}

void EventLoopManagerGLib::unregister_signal(int signal_id)
{
    auto id = static_cast<guint>(signal_id);

    auto* source = g_main_context_find_source_by_id(get_main_context(), id);
    if (source) {
        g_source_destroy(source);
        g_source_unref(source);
    } else {
        dbgln("EventLoopManagerGLib::unregister_signal: No source found for glib tag {}", signal_id);
    }
}

NonnullOwnPtr<EventLoopImplementationGLib> EventLoopImplementationGLib::create()
{
    return adopt_own(*new EventLoopImplementationGLib());
}

static bool s_created_main_loop = false;

EventLoopImplementationGLib::EventLoopImplementationGLib()
    : m_context(nullptr)
    , m_core_event_source(CoreEventSource::create())
{
    if (!s_created_main_loop) {
        m_context = Glib::MainContext::get_default();
        s_created_main_loop = true;
    } else {
        // We want to be able to nest Core::EventLoops, so always create a new context if we're not the main thread loop.
        m_context = Glib::MainContext::create();
        m_context->push_thread_default();
    }

    m_core_event_source->attach(m_context);
}

EventLoopImplementationGLib::~EventLoopImplementationGLib()
{
    m_core_event_source->destroy();
    m_core_event_source = nullptr;

    if (m_context == Glib::MainContext::get_default()) {
        s_created_main_loop = false;
    } else {
        // If we're not the main loop, we need to pop the context.
        m_context->pop_thread_default();
    }
}

int EventLoopImplementationGLib::exec()
{
    while (!m_should_quit)
        m_context->iteration(true);
    return m_exit_code;
}

void EventLoopImplementationGLib::quit(int code)
{
    m_exit_code = code;
    m_should_quit = true;
}

size_t EventLoopImplementationGLib::pump(PumpMode pump_mode)
{
    bool may_block = (pump_mode == PumpMode::WaitForEvents);
    m_context->iteration(may_block);
    return 0;
}

void EventLoopImplementationGLib::wake()
{
    m_context->wakeup();
}

bool EventLoopImplementationGLib::was_exit_requested() const
{
    return m_should_quit;
}

}
