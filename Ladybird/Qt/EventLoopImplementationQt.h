/*
 * Copyright (c) 2022-2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <LibCore/EventLoopImplementation.h>
#include <QEvent>
#include <QEventLoop>
#include <QSocketNotifier>
#include <QTimer>

namespace Ladybird {

class EventLoopImplementationQt;
class EventLoopImplementationQtEventTarget;

class EventLoopManagerQt final : public Core::EventLoopManager {
public:
    EventLoopManagerQt();
    virtual ~EventLoopManagerQt() override;
    virtual NonnullOwnPtr<Core::EventLoopImplementation> make_implementation() override;

    virtual intptr_t register_timer(Core::EventReceiver&, int milliseconds, bool should_reload, Core::TimerShouldFireWhenNotVisible) override;
    virtual void unregister_timer(intptr_t timer_id) override;

    virtual void register_notifier(Core::Notifier&) override;
    virtual void unregister_notifier(Core::Notifier&) override;

    virtual void did_post_event() override;
    static bool event_target_received_event(Badge<EventLoopImplementationQtEventTarget>, QEvent* event);

    virtual int register_signal(int, Function<void(int)>) override;
    virtual void unregister_signal(int) override;

    void set_main_loop_signal_notifiers(Badge<EventLoopImplementationQt>);

private:
    static void handle_signal(int);

    NonnullOwnPtr<EventLoopImplementationQtEventTarget> m_main_thread_event_target;
    QSocketNotifier* m_signal_socket_notifier { nullptr };
    int m_signal_socket_fds[2] = { -1, -1 };
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

class EventLoopImplementationQt final : public Core::EventLoopImplementation {
public:
    static NonnullOwnPtr<EventLoopImplementationQt> create() { return adopt_own(*new EventLoopImplementationQt); }

    virtual ~EventLoopImplementationQt() override;

    virtual int exec() override;
    virtual size_t pump(PumpMode) override;
    virtual void quit(int) override;
    virtual void wake() override;
    virtual void post_event(Core::EventReceiver& receiver, NonnullOwnPtr<Core::Event>&&) override;

    // FIXME: These APIs only exist for obscure use-cases inside SerenityOS. Try to get rid of them.
    virtual void unquit() override { }
    virtual bool was_exit_requested() const override { return false; }
    virtual void notify_forked_and_in_child() override { }

    void set_main_loop();

private:
    friend class EventLoopManagerQt;

    EventLoopImplementationQt();
    bool is_main_loop() const { return m_main_loop; }

    QEventLoop m_event_loop;
    bool m_main_loop { false };
};

}
