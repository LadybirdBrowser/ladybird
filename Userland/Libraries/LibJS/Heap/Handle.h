/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/IntrusiveList.h>
#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/SourceLocation.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class HandleImpl : public RefCounted<HandleImpl> {
    AK_MAKE_NONCOPYABLE(HandleImpl);
    AK_MAKE_NONMOVABLE(HandleImpl);

public:
    ~HandleImpl();

    Cell* cell() { return m_cell; }
    Cell const* cell() const { return m_cell; }

    SourceLocation const& source_location() const { return m_location; }

private:
    template<class T>
    friend class Handle;

    explicit HandleImpl(Cell*, SourceLocation location);
    GCPtr<Cell> m_cell;
    SourceLocation m_location;

    IntrusiveListNode<HandleImpl> m_list_node;

public:
    using List = IntrusiveList<&HandleImpl::m_list_node>;
};

template<class T>
class Handle {
public:
    Handle() = default;

    static Handle create(T* cell, SourceLocation location = SourceLocation::current())
    {
        return Handle(adopt_ref(*new HandleImpl(const_cast<RemoveConst<T>*>(cell), location)));
    }

    Handle(T* cell, SourceLocation location = SourceLocation::current())
    {
        if (cell)
            m_impl = adopt_ref(*new HandleImpl(cell, location));
    }

    Handle(T& cell, SourceLocation location = SourceLocation::current())
        : m_impl(adopt_ref(*new HandleImpl(&cell, location)))
    {
    }

    Handle(GCPtr<T> cell, SourceLocation location = SourceLocation::current())
        : Handle(cell.ptr(), location)
    {
    }

    Handle(NonnullGCPtr<T> cell, SourceLocation location = SourceLocation::current())
        : Handle(*cell, location)
    {
    }

    T* cell() const
    {
        if (!m_impl)
            return nullptr;
        return static_cast<T*>(m_impl->cell());
    }

    T* ptr() const
    {
        return cell();
    }

    bool is_null() const
    {
        return m_impl.is_null();
    }

    T* operator->() const
    {
        return cell();
    }

    [[nodiscard]] T& operator*() const
    {
        return *cell();
    }

    bool operator!() const
    {
        return !cell();
    }
    operator bool() const
    {
        return cell();
    }

    operator T*() const { return cell(); }

private:
    explicit Handle(NonnullRefPtr<HandleImpl> impl)
        : m_impl(move(impl))
    {
    }

    RefPtr<HandleImpl> m_impl;
};

template<class T>
inline Handle<T> make_handle(T* cell, SourceLocation location = SourceLocation::current())
{
    if (!cell)
        return Handle<T> {};
    return Handle<T>::create(cell, location);
}

template<class T>
inline Handle<T> make_handle(T& cell, SourceLocation location = SourceLocation::current())
{
    return Handle<T>::create(&cell, location);
}

template<class T>
inline Handle<T> make_handle(GCPtr<T> cell, SourceLocation location = SourceLocation::current())
{
    if (!cell)
        return Handle<T> {};
    return Handle<T>::create(cell.ptr(), location);
}

template<class T>
inline Handle<T> make_handle(NonnullGCPtr<T> cell, SourceLocation location = SourceLocation::current())
{
    return Handle<T>::create(cell.ptr(), location);
}

template<>
class Handle<Value> {
public:
    Handle() = default;

    static Handle create(Value value, SourceLocation location)
    {
        if (value.is_cell())
            return Handle(value, &value.as_cell(), location);
        return Handle(value);
    }

    auto cell() { return m_handle.cell(); }
    auto cell() const { return m_handle.cell(); }
    auto value() const { return *m_value; }
    bool is_null() const { return m_handle.is_null() && !m_value.has_value(); }

    bool operator==(Value const& value) const { return value == m_value; }
    bool operator==(Handle<Value> const& other) const { return other.m_value == this->m_value; }

private:
    explicit Handle(Value value)
        : m_value(value)
    {
    }

    explicit Handle(Value value, Cell* cell, SourceLocation location)
        : m_value(value)
        , m_handle(Handle<Cell>::create(cell, location))
    {
    }

    Optional<Value> m_value;
    Handle<Cell> m_handle;
};

inline Handle<Value> make_handle(Value value, SourceLocation location = SourceLocation::current())
{
    return Handle<Value>::create(value, location);
}

}

namespace AK {

template<typename T>
struct Traits<JS::Handle<T>> : public DefaultTraits<JS::Handle<T>> {
    static unsigned hash(JS::Handle<T> const& handle) { return Traits<T>::hash(handle); }
};

template<>
struct Traits<JS::Handle<JS::Value>> : public DefaultTraits<JS::Handle<JS::Value>> {
    static unsigned hash(JS::Handle<JS::Value> const& handle) { return Traits<JS::Value>::hash(handle.value()); }
};

namespace Detail {
template<typename T>
inline constexpr bool IsHashCompatible<JS::Handle<T>, T> = true;

template<typename T>
inline constexpr bool IsHashCompatible<T, JS::Handle<T>> = true;

}
}
