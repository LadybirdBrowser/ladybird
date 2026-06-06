/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Vector.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Object.h>

#define DEFINE_CACHED_ATTRIBUTE(name)                                                                       \
    GC::Ptr<JS::Object> cached_##name(JS::Realm& realm) const                                               \
    {                                                                                                       \
        if (&realm == &this->realm())                                                                       \
            return m_cached_##name.ptr();                                                                   \
                                                                                                            \
        m_live_cached_##name.remove_all_matching([](auto const& cached_##name) { return !cached_##name; }); \
        for (auto const& cached_##name : m_live_cached_##name) {                                            \
            if (&cached_##name->shape().realm() == &realm)                                                  \
                return cached_##name.ptr();                                                                 \
        }                                                                                                   \
                                                                                                            \
        return nullptr;                                                                                     \
    }                                                                                                       \
    void set_cached_##name(JS::Realm& realm, GC::Ptr<JS::Object> cached_##name)                             \
    {                                                                                                       \
        if (!cached_##name) {                                                                               \
            clear_cached_##name();                                                                          \
            return;                                                                                         \
        }                                                                                                   \
                                                                                                            \
        if (&realm == &this->realm()) {                                                                     \
            m_cached_##name = cached_##name.ptr();                                                          \
            return;                                                                                         \
        }                                                                                                   \
                                                                                                            \
        m_live_cached_##name.remove_all_matching([&realm](auto const& cached_##name) {                      \
            return !cached_##name || &cached_##name->shape().realm() == &realm;                             \
        });                                                                                                 \
        m_live_cached_##name.append(cached_##name.ptr());                                                   \
    }                                                                                                       \
    void set_cached_##name(GC::Ptr<JS::Object> cached_##name)                                               \
    {                                                                                                       \
        VERIFY(!cached_##name);                                                                             \
        clear_cached_##name();                                                                              \
    }                                                                                                       \
    void clear_cached_##name()                                                                              \
    {                                                                                                       \
        m_cached_##name = nullptr;                                                                          \
        m_live_cached_##name.clear();                                                                       \
    }                                                                                                       \
                                                                                                            \
private:                                                                                                    \
    mutable GC::Weak<JS::Object> m_cached_##name;                                                           \
    mutable Vector<GC::Weak<JS::Object>> m_live_cached_##name;                                              \
                                                                                                            \
public:

#define VISIT_CACHED_ATTRIBUTE(name)
