/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/WrapperWorld.h>

#define DEFINE_CACHED_ATTRIBUTE(name)                                                         \
    GC::Ptr<JS::Object> cached_##name(Web::Bindings::WrapperWorld const& wrapper_world) const \
    {                                                                                         \
        return m_cached_##name.get(wrapper_world);                                            \
    }                                                                                         \
    void set_cached_##name(Web::Bindings::WrapperWorld const& wrapper_world,                  \
        GC::Ptr<JS::Object> cached_##name)                                                    \
    {                                                                                         \
        if (!cached_##name) {                                                                 \
            clear_cached_##name();                                                            \
            return;                                                                           \
        }                                                                                     \
                                                                                              \
        m_cached_##name.set(wrapper_world, cached_##name);                                    \
    }                                                                                         \
    void set_cached_##name(GC::Ptr<JS::Object> cached_##name)                                 \
    {                                                                                         \
        VERIFY(!cached_##name);                                                               \
        clear_cached_##name();                                                                \
    }                                                                                         \
    void clear_cached_##name()                                                                \
    {                                                                                         \
        m_cached_##name.clear();                                                              \
    }                                                                                         \
                                                                                              \
private:                                                                                      \
    mutable Web::Bindings::WrapperWorldWeakValueCache<JS::Object> m_cached_##name;            \
                                                                                              \
public:

#define VISIT_CACHED_ATTRIBUTE(name)
