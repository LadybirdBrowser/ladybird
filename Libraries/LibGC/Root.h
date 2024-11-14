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
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>

namespace GC {

class RootImpl : public RefCounted<RootImpl> {
    AK_MAKE_NONCOPYABLE(RootImpl);
    AK_MAKE_NONMOVABLE(RootImpl);

public:
    ~RootImpl();

    Cell* cell() { return m_cell; }
    Cell const* cell() const { return m_cell; }

    SourceLocation const& source_location() const { return m_location; }

private:
    template<class T>
    friend class Root;

    explicit RootImpl(Cell*, SourceLocation location);
    Ptr<Cell> m_cell;
    SourceLocation m_location;

    IntrusiveListNode<RootImpl> m_list_node;

public:
    using List = IntrusiveList<&RootImpl::m_list_node>;
};

template<class T>
class Root {
public:
    Root() = default;

    static Root create(T* cell, SourceLocation location = SourceLocation::current())
    {
        return Root(adopt_ref(*new RootImpl(const_cast<RemoveConst<T>*>(cell), location)));
    }

    Root(T* cell, SourceLocation location = SourceLocation::current())
    {
        if (cell)
            m_impl = adopt_ref(*new RootImpl(cell, location));
    }

    Root(T& cell, SourceLocation location = SourceLocation::current())
        : m_impl(adopt_ref(*new RootImpl(&cell, location)))
    {
    }

    Root(Ptr<T> cell, SourceLocation location = SourceLocation::current())
        : Root(cell.ptr(), location)
    {
    }

    Root(Ref<T> cell, SourceLocation location = SourceLocation::current())
        : Root(*cell, location)
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
    explicit Root(NonnullRefPtr<RootImpl> impl)
        : m_impl(move(impl))
    {
    }

    RefPtr<RootImpl> m_impl;
};

template<class T>
inline Root<T> make_root(T* cell, SourceLocation location = SourceLocation::current())
{
    if (!cell)
        return Root<T> {};
    return Root<T>::create(cell, location);
}

template<class T>
inline Root<T> make_root(T& cell, SourceLocation location = SourceLocation::current())
{
    return Root<T>::create(&cell, location);
}

template<class T>
inline Root<T> make_root(Ptr<T> cell, SourceLocation location = SourceLocation::current())
{
    if (!cell)
        return Root<T> {};
    return Root<T>::create(cell.ptr(), location);
}

template<class T>
inline Root<T> make_root(Ref<T> cell, SourceLocation location = SourceLocation::current())
{
    return Root<T>::create(cell.ptr(), location);
}

}

namespace AK {

template<typename T>
struct Traits<GC::Root<T>> : public DefaultTraits<GC::Root<T>> {
    static unsigned hash(GC::Root<T> const& handle) { return Traits<T>::hash(handle); }
};

namespace Detail {
template<typename T>
inline constexpr bool IsHashCompatible<GC::Root<T>, T> = true;

template<typename T>
inline constexpr bool IsHashCompatible<T, GC::Root<T>> = true;

}
}
