/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/StdLibExtras.h>
#include <LibSync/Mutex.h>

namespace Sync {

template<typename T>
class Weakable;

class WeakRefLink : public AtomicRefCounted<WeakRefLink> {
    template<typename T>
    friend class Weakable;

public:
    template<typename T>
    requires(IsBaseOf<AtomicRefCountedBase, T>)
    RefPtr<T> strong_ref()
    {
        MutexLocker locker { m_mutex };
        auto* target = static_cast<T*>(m_target);
        if (target == nullptr || !target->try_ref())
            return {};
        return adopt_ref(*target);
    }

    // Runs the callback under the link's mutex: revocation blocks until in-flight callbacks return,
    // so the callback must not block or acquire locks held around revocation.
    template<typename T, typename F>
    void with_target(F callback)
    {
        MutexLocker locker { m_mutex };
        if (auto* target = static_cast<T*>(m_target))
            callback(*target);
    }

private:
    explicit WeakRefLink(void* target)
        : m_target(target)
    {
    }

    void revoke()
    {
        MutexLocker locker { m_mutex };
        m_target = nullptr;
    }

    Mutex m_mutex;
    void* m_target { nullptr };
};

template<typename T>
class WeakRef {
public:
    WeakRef() = default;
    WeakRef(RefPtr<WeakRefLink> link)
        : m_link(move(link))
    {
    }

    RefPtr<T> strong_ref() const
    {
        if (!m_link)
            return {};
        return m_link->template strong_ref<T>();
    }

    template<typename F>
    void with_target(F callback) const
    {
        if (m_link)
            m_link->template with_target<T>(move(callback));
    }

private:
    RefPtr<WeakRefLink> m_link;
};

template<typename T>
class Weakable {
public:
    WeakRef<T> make_weak_ref()
    {
        return WeakRef<T> { m_link };
    }

protected:
    Weakable()
        : m_link(adopt_ref(*new WeakRefLink(static_cast<T*>(this))))
    {
    }

    ~Weakable() { m_link->revoke(); }

    // ~Weakable() revokes only after the derived object's members are already destroyed. If with_target() is to be
    // used, this should be called before any destruction that breaks functionality of the object.
    void revoke_weak_refs() { m_link->revoke(); }

private:
    NonnullRefPtr<WeakRefLink> m_link;
};

}
