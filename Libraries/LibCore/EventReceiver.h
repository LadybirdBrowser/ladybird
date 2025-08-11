/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>

namespace Core {

enum class TimerShouldFireWhenNotVisible {
    No = 0,
    Yes
};

#define C_OBJECT(klass)                                                                    \
public:                                                                                    \
    virtual StringView class_name() const override                                         \
    {                                                                                      \
        return #klass##sv;                                                                 \
    }                                                                                      \
    template<typename Klass = klass, class... Args>                                        \
    static NonnullRefPtr<klass> construct(Args&&... args)                                  \
    {                                                                                      \
        return adopt_ref(*new Klass(::forward<Args>(args)...));                            \
    }                                                                                      \
    template<typename Klass = klass, class... Args>                                        \
    static ErrorOr<NonnullRefPtr<klass>> try_create(Args&&... args)                        \
    {                                                                                      \
        return adopt_nonnull_ref_or_enomem(new (nothrow) Klass(::forward<Args>(args)...)); \
    }

#define C_OBJECT_ABSTRACT(klass)                   \
public:                                            \
    virtual StringView class_name() const override \
    {                                              \
        return #klass##sv;                         \
    }

class EventReceiver
    : public AtomicRefCounted<EventReceiver>
    , public Weakable<EventReceiver> {
    // NOTE: No C_OBJECT macro for Core::EventReceiver itself.

    AK_MAKE_NONCOPYABLE(EventReceiver);
    AK_MAKE_NONMOVABLE(EventReceiver);

public:
    virtual ~EventReceiver();

    virtual StringView class_name() const = 0;

    template<typename T>
    bool fast_is() const = delete;

    void start_timer(int ms, TimerShouldFireWhenNotVisible = TimerShouldFireWhenNotVisible::No);
    void stop_timer();
    bool has_timer() const { return m_timer_id; }

    void deferred_invoke(Function<void()>);

    void dispatch_event(Core::Event&);

    virtual bool is_visible_for_timer_purposes() const;

protected:
    EventReceiver();

    virtual void event(Core::Event&);

    virtual void timer_event(TimerEvent&);
    virtual void custom_event(CustomEvent&);

private:
    intptr_t m_timer_id { 0 };
};

}

template<>
struct AK::Formatter<Core::EventReceiver> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Core::EventReceiver const& value)
    {
        return AK::Formatter<FormatString>::format(builder, "{}({})"sv, value.class_name(), &value);
    }
};
