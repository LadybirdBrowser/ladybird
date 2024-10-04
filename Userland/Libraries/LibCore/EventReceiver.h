/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/TypeCasts.h>
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
    : public RefCounted<EventReceiver>
    , public Weakable<EventReceiver> {
    // NOTE: No C_OBJECT macro for Core::EventReceiver itself.

    AK_MAKE_NONCOPYABLE(EventReceiver);
    AK_MAKE_NONMOVABLE(EventReceiver);

public:
    virtual ~EventReceiver();

    virtual StringView class_name() const = 0;

    template<typename T>
    bool fast_is() const = delete;

    virtual bool is_widget() const { return false; }

    ByteString const& name() const { return m_name; }
    void set_name(ByteString name) { m_name = move(name); }

    Vector<NonnullRefPtr<EventReceiver>>& children() { return m_children; }
    Vector<NonnullRefPtr<EventReceiver>> const& children() const { return m_children; }

    template<typename Callback>
    void for_each_child(Callback callback)
    {
        for (auto& child : m_children) {
            if (callback(*child) == IterationDecision::Break)
                return;
        }
    }

    template<typename T, typename Callback>
    void for_each_child_of_type(Callback callback)
    requires IsBaseOf<EventReceiver, T>;

    template<typename T>
    T* find_child_of_type_named(StringView)
    requires IsBaseOf<EventReceiver, T>;

    template<typename T, size_t N>
    ALWAYS_INLINE T* find_child_of_type_named(char const (&string_literal)[N])
    requires IsBaseOf<EventReceiver, T>
    {
        return find_child_of_type_named<T>(StringView { string_literal, N - 1 });
    }

    template<typename T>
    T* find_descendant_of_type_named(StringView)
    requires IsBaseOf<EventReceiver, T>;

    template<typename T, size_t N>
    ALWAYS_INLINE T* find_descendant_of_type_named(char const (&string_literal)[N])
    requires IsBaseOf<EventReceiver, T>
    {
        return find_descendant_of_type_named<T>(StringView { string_literal, N - 1 });
    }

    bool is_ancestor_of(EventReceiver const&) const;

    EventReceiver* parent() { return m_parent; }
    EventReceiver const* parent() const { return m_parent; }

    void start_timer(int ms, TimerShouldFireWhenNotVisible = TimerShouldFireWhenNotVisible::No);
    void stop_timer();
    bool has_timer() const { return m_timer_id; }

    ErrorOr<void> try_add_child(EventReceiver&);

    void add_child(EventReceiver&);
    void insert_child_before(EventReceiver& new_child, EventReceiver& before_child);
    void remove_child(EventReceiver&);
    void remove_all_children();

    void set_event_filter(Function<bool(Core::Event&)>);

    void deferred_invoke(Function<void()>);

    void dispatch_event(Core::Event&, EventReceiver* stay_within = nullptr);

    void remove_from_parent()
    {
        if (m_parent)
            m_parent->remove_child(*this);

        // The call to `remove_child` may have deleted the object.
        // Do not dereference `this` from this point forward.
    }

    template<class T, class... Args>
    inline T& add(Args&&... args)
    {
        auto child = T::construct(forward<Args>(args)...);
        add_child(*child);
        return child;
    }

    template<class T, class... Args>
    inline ErrorOr<NonnullRefPtr<T>> try_add(Args&&... args)
    {
        auto child = TRY(T::try_create(forward<Args>(args)...));
        TRY(try_add_child(*child));
        return child;
    }

    virtual bool is_visible_for_timer_purposes() const;

protected:
    explicit EventReceiver(EventReceiver* parent = nullptr);

    virtual void event(Core::Event&);

    virtual void timer_event(TimerEvent&);
    virtual void custom_event(CustomEvent&);

    // NOTE: You may get child events for children that are not yet fully constructed!
    virtual void child_event(ChildEvent&);

private:
    EventReceiver* m_parent { nullptr };
    ByteString m_name;
    intptr_t m_timer_id { 0 };
    Vector<NonnullRefPtr<EventReceiver>> m_children;
    Function<bool(Core::Event&)> m_event_filter;
};

}

template<>
struct AK::Formatter<Core::EventReceiver> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Core::EventReceiver const& value)
    {
        return AK::Formatter<FormatString>::format(builder, "{}({})"sv, value.class_name(), &value);
    }
};

namespace Core {
template<typename T, typename Callback>
inline void EventReceiver::for_each_child_of_type(Callback callback)
requires IsBaseOf<EventReceiver, T>
{
    for_each_child([&](auto& child) {
        if (is<T>(child))
            return callback(static_cast<T&>(child));
        return IterationDecision::Continue;
    });
}

template<typename T>
T* EventReceiver::find_child_of_type_named(StringView name)
requires IsBaseOf<EventReceiver, T>
{
    T* found_child = nullptr;
    for_each_child_of_type<T>([&](auto& child) {
        if (child.name() == name) {
            found_child = &child;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });

    return found_child;
}

template<typename T>
T* EventReceiver::find_descendant_of_type_named(StringView name)
requires IsBaseOf<EventReceiver, T>
{
    if (is<T>(*this) && this->name() == name) {
        return static_cast<T*>(this);
    }
    T* found_child = nullptr;
    for_each_child([&](auto& child) {
        found_child = child.template find_descendant_of_type_named<T>(name);
        if (found_child)
            return IterationDecision::Break;
        return IterationDecision::Continue;
    });
    return found_child;
}

}
