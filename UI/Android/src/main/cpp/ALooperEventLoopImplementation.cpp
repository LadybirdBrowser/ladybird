/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ALooperEventLoopImplementation.h"
#include "JNIHelpers.h"
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/RWLock.h>
#include <android/log.h>
#include <android/looper.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>

namespace Ladybird {

static thread_local OwnPtr<EventLoopThreadData> s_this_thread_data;
static HashMap<pthread_t, EventLoopThreadData*> s_thread_data;
static Threading::RWLock s_thread_data_lock;
static thread_local Optional<pthread_t> s_thread_id;

EventLoopThreadData& EventLoopThreadData::the()
{
    if (!s_thread_id.has_value())
        s_thread_id = pthread_self();
    if (!s_this_thread_data) {
        s_this_thread_data = make<EventLoopThreadData>();
        s_this_thread_data->thread_id = s_thread_id.value();
        Threading::RWLockLocker<Threading::LockMode::Write> locker(s_thread_data_lock);
        s_thread_data.set(s_thread_id.value(), s_this_thread_data.ptr());
    }
    return *s_this_thread_data;
}

EventLoopThreadData* EventLoopThreadData::for_thread(pthread_t thread_id)
{
    Threading::RWLockLocker<Threading::LockMode::Read> locker(s_thread_data_lock);
    return s_thread_data.get(thread_id).value_or(nullptr);
}

EventLoopThreadData::~EventLoopThreadData()
{
    Threading::RWLockLocker<Threading::LockMode::Write> locker(s_thread_data_lock);
    s_thread_data.remove(thread_id);
}

static ALooperEventLoopImplementation& current_impl()
{
    return as<ALooperEventLoopImplementation>(Core::EventLoop::current().impl());
}

static int looper_callback(int fd, int events, void* data);

ALooperEventLoopManager::ALooperEventLoopManager(jobject timer_service)
    : m_timer_service(timer_service)
{
    JavaEnvironment env(global_vm);

    jclass timer_class = env.get()->FindClass("org/serenityos/ladybird/TimerExecutorService$Timer");
    if (!timer_class)
        TODO();
    m_timer_class = reinterpret_cast<jclass>(env.get()->NewGlobalRef(timer_class));
    env.get()->DeleteLocalRef(timer_class);

    m_timer_constructor = env.get()->GetMethodID(m_timer_class, "<init>", "(J)V");
    if (!m_timer_constructor)
        TODO();

    jclass timer_service_class = env.get()->GetObjectClass(m_timer_service);

    m_register_timer = env.get()->GetMethodID(timer_service_class, "registerTimer", "(Lorg/serenityos/ladybird/TimerExecutorService$Timer;ZJ)J");
    if (!m_register_timer)
        TODO();

    m_unregister_timer = env.get()->GetMethodID(timer_service_class, "unregisterTimer", "(J)V");
    if (!m_unregister_timer)
        TODO();
    env.get()->DeleteLocalRef(timer_service_class);

    auto ret = pipe2(m_pipe, O_CLOEXEC | O_NONBLOCK);
    VERIFY(ret == 0);

    m_main_looper = ALooper_forThread();
    VERIFY(m_main_looper);
    ALooper_acquire(m_main_looper);

    ret = ALooper_addFd(m_main_looper, m_pipe[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, &looper_callback, this);
    VERIFY(ret == 1);
}

ALooperEventLoopManager::~ALooperEventLoopManager()
{
    JavaEnvironment env(global_vm);

    env.get()->DeleteGlobalRef(m_timer_service);
    env.get()->DeleteGlobalRef(m_timer_class);

    ALooper_removeFd(m_main_looper, m_pipe[0]);
    ALooper_release(m_main_looper);

    ::close(m_pipe[0]);
    ::close(m_pipe[1]);
}

NonnullOwnPtr<Core::EventLoopImplementation> ALooperEventLoopManager::make_implementation()
{
    return ALooperEventLoopImplementation::create();
}

intptr_t ALooperEventLoopManager::register_timer(Core::EventReceiver& receiver, int milliseconds, bool should_reload)
{
    JavaEnvironment env(global_vm);
    auto timer = env.get()->NewObject(m_timer_class, m_timer_constructor, reinterpret_cast<long>(this));

    long millis = milliseconds;
    long timer_id = env.get()->CallLongMethod(m_timer_service, m_register_timer, timer, !should_reload, millis);

    {
        Threading::MutexLocker locker(m_timers_lock);
        m_timers.set(timer_id, { receiver.make_weak_ptr(), Core::EventLoop::current_weak() });
    }

    return timer_id;
}

void ALooperEventLoopManager::timer_fired(long timer_id)
{
    Optional<TimerData> timer_data;
    {
        Threading::MutexLocker locker(m_timers_lock);
        auto timer_data_in_map = m_timers.get(timer_id);
        if (timer_data_in_map.has_value())
            timer_data = timer_data_in_map.copy();
    }

    if (!timer_data.has_value())
        return;

    auto receiver = timer_data->receiver.strong_ref();
    if (!receiver) {
        unregister_timer(timer_id);
        return;
    }

    auto event_loop = timer_data->event_loop->take();
    if (!event_loop || !*event_loop) {
        unregister_timer(timer_id);
        return;
    }

    auto& impl = as<ALooperEventLoopImplementation>((*event_loop)->impl());
    impl.post_event(*receiver, Core::Event::Type::Timer);
}

void ALooperEventLoopManager::unregister_timer(intptr_t timer_id)
{
    bool did_remove = false;
    {
        Threading::MutexLocker locker(m_timers_lock);
        did_remove = m_timers.remove(timer_id);
    }

    if (did_remove) {
        JavaEnvironment env(global_vm);
        env.get()->CallVoidMethod(m_timer_service, m_unregister_timer, timer_id);
    }
}

void ALooperEventLoopManager::register_notifier(Core::Notifier& notifier)
{
    auto& thread_data = EventLoopThreadData::the();
    {
        Threading::MutexLocker locker(thread_data.mutex);
        thread_data.notifiers.set(&notifier);
    }

    current_impl().register_notifier(notifier);
    notifier.set_owner_thread(thread_data.thread_id);
}

void ALooperEventLoopManager::unregister_notifier(Core::Notifier& notifier)
{
    auto* thread_data = EventLoopThreadData::for_thread(notifier.owner_thread());
    if (!thread_data)
        return;

    ALooperEventLoopImplementation* event_loop_impl = nullptr;
    {
        Threading::MutexLocker locker(thread_data->mutex);
        if (!thread_data->notifiers.remove(&notifier))
            return;
        event_loop_impl = thread_data->event_loop_impl;
    }

    if (event_loop_impl)
        event_loop_impl->unregister_notifier(notifier);
}

void ALooperEventLoopManager::did_post_event()
{
    int msg = 0xCAFEBABE;
    (void)write(m_pipe[1], &msg, sizeof(msg));
}

int looper_callback(int fd, int events, void* data)
{
    auto& manager = *static_cast<ALooperEventLoopManager*>(data);

    if (events & ALOOPER_EVENT_INPUT) {
        int msg = 0;
        while (read(fd, &msg, sizeof(msg)) == sizeof(msg)) {
            // Do nothing, we don't actually care what the message was, just that it was posted
        }
        if (manager.on_did_post_event)
            manager.on_did_post_event();
    }
    return 1;
}

ALooperEventLoopImplementation::ALooperEventLoopImplementation()
    : m_event_loop(ALooper_prepare(0))
    , m_thread_data(&EventLoopThreadData::the())
{
    {
        Threading::MutexLocker locker(m_thread_data->mutex);
        m_thread_data->event_loop_impl = this;
    }

    ALooper_acquire(m_event_loop);
}

ALooperEventLoopImplementation::~ALooperEventLoopImplementation()
{
    {
        Threading::MutexLocker locker(m_thread_data->mutex);
        if (m_thread_data->event_loop_impl == this)
            m_thread_data->event_loop_impl = nullptr;
    }

    ALooper_release(m_event_loop);
}

EventLoopThreadData& ALooperEventLoopImplementation::thread_data()
{
    return *m_thread_data;
}

int ALooperEventLoopImplementation::exec()
{
    while (!m_exit_requested.load(MemoryOrder::memory_order_acquire))
        pump(PumpMode::WaitForEvents);
    return m_exit_code;
}

size_t ALooperEventLoopImplementation::pump(Core::EventLoopImplementation::PumpMode mode)
{
    auto num_events = Core::ThreadEventQueue::current().process();

    int timeout_ms = mode == Core::EventLoopImplementation::PumpMode::WaitForEvents ? -1 : 0;
    int ret;
    do {
        ret = ALooper_pollOnce(timeout_ms, nullptr, nullptr, nullptr);
    } while (ret == ALOOPER_POLL_CALLBACK);

    // We don't expect any non-callback FDs to be ready
    VERIFY(ret <= 0);

    if (ret == ALOOPER_POLL_ERROR)
        m_exit_requested.store(true, MemoryOrder::memory_order_release);

    num_events += Core::ThreadEventQueue::current().process();
    return num_events;
}

void ALooperEventLoopImplementation::quit(int code)
{
    m_exit_code = code;
    m_exit_requested.store(true, MemoryOrder::memory_order_release);
    wake();
}

void ALooperEventLoopImplementation::wake()
{
    ALooper_wake(m_event_loop);
}

void ALooperEventLoopImplementation::post_event(Core::EventReceiver& receiver, Core::Event::Type event_type)
{
    m_thread_event_queue.post_event(&receiver, event_type);

    if (&m_thread_event_queue != &Core::ThreadEventQueue::current())
        wake();
}

static int notifier_callback(int fd, int events, void* data)
{
    auto& notifier = *static_cast<Core::Notifier*>(data);

    VERIFY(fd == notifier.fd());

    Core::NotificationType type = Core::NotificationType::None;
    if (events & ALOOPER_EVENT_INPUT)
        type |= Core::NotificationType::Read;
    if (events & ALOOPER_EVENT_OUTPUT)
        type |= Core::NotificationType::Write;
    if (events & ALOOPER_EVENT_HANGUP)
        type |= Core::NotificationType::HangUp;
    if (events & ALOOPER_EVENT_ERROR)
        type |= Core::NotificationType::Error;

    if (type != Core::NotificationType::None) {
        Core::NotifierActivationEvent event;
        notifier.dispatch_event(event);
    }

    // Wake up from ALooper_pollAll, and service this event on the event queue
    current_impl().wake();

    return 1;
}

void ALooperEventLoopImplementation::register_notifier(Core::Notifier& notifier)
{
    auto event_flags = 0;
    switch (notifier.type()) {
    case Core::Notifier::Type::Read:
        event_flags = ALOOPER_EVENT_INPUT;
        break;
    case Core::Notifier::Type::Write:
        event_flags = ALOOPER_EVENT_OUTPUT;
        break;
    case Core::Notifier::Type::Error:
        event_flags = ALOOPER_EVENT_ERROR;
        break;
    case Core::Notifier::Type::HangUp:
        event_flags = ALOOPER_EVENT_HANGUP;
        break;
    case Core::Notifier::Type::None:
        TODO();
    }

    auto ret = ALooper_addFd(m_event_loop, notifier.fd(), ALOOPER_POLL_CALLBACK, event_flags, &notifier_callback, &notifier);
    VERIFY(ret == 1);
}

void ALooperEventLoopImplementation::unregister_notifier(Core::Notifier& notifier)
{
    ALooper_removeFd(m_event_loop, notifier.fd());
}

}
