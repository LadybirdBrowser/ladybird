/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Function.h>
#include <AK/Types.h>
#include <AK/WeakPtr.h>
#include <LibCore/DeferredInvocationContext.h>
#include <LibCore/Forward.h>

namespace Core {

class Event {
public:
    enum Type : u8 {
        Invalid = 0,
        Quit,
        Timer,
        NotifierActivation,
        DeferredInvoke,
    };

    Event() = default;
    explicit Event(unsigned type)
        : m_type(type)
    {
    }
    virtual ~Event() = default;

    unsigned type() const { return m_type; }

    bool is_accepted() const { return m_accepted; }
    void accept() { m_accepted = true; }
    void ignore() { m_accepted = false; }

private:
    unsigned m_type { Type::Invalid };
    bool m_accepted { true };
};

class DeferredInvocationEvent : public Event {
    friend class EventLoop;
    friend class ThreadEventQueue;

public:
    DeferredInvocationEvent(NonnullRefPtr<DeferredInvocationContext> context, Function<void()> invokee)
        : Event(Event::Type::DeferredInvoke)
        , m_context(move(context))
        , m_invokee(move(invokee))
    {
    }

private:
    NonnullRefPtr<DeferredInvocationContext> m_context;
    Function<void()> m_invokee;
};

class TimerEvent final : public Event {
public:
    explicit TimerEvent()
        : Event(Event::Timer)
    {
    }

    ~TimerEvent() = default;
};

enum class NotificationType : u8 {
    None = 0,
    Read = 1,
    Write = 2,
    HangUp = 4,
    Error = 8,
};

AK_ENUM_BITWISE_OPERATORS(NotificationType);

class NotifierActivationEvent final : public Event {
public:
    explicit NotifierActivationEvent(int fd, NotificationType type)
        : Event(Event::NotifierActivation)
        , m_fd(fd)
        , m_type(type)
    {
    }
    ~NotifierActivationEvent() = default;

    int fd() const { return m_fd; }
    NotificationType type() const { return m_type; }

private:
    int m_fd;
    NotificationType m_type;
};

}
