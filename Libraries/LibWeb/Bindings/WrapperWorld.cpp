/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(WrapperWorld);

static void verify_cache_entry(WrapperWorld const& world, Wrappable const& wrappable, PlatformObject const& wrapper)
{
    VERIFY(wrapper.realm().host_defined());
    VERIFY(&host_defined_wrapper_world(wrapper.realm()) == &world);
    VERIFY(wrappable_impl_from(&wrapper) == &wrappable);
}

WrapperWorld::WrapperWorld(Type type)
    : m_type(type)
{
}

WrapperWorld::~WrapperWorld() = default;

void WrapperWorld::finalize()
{
    Base::finalize();
    VERIFY(is_main_world() || is_detached() || m_preserved_wrappables.is_empty() || heap().is_collecting_everything());
}

void WrapperWorld::detach()
{
    VERIFY(!is_main_world());
    if (m_detached)
        return;
    m_detached = true;
    for (auto& wrappable : m_preserved_wrappables)
        wrappable.clear_preserved_wrappers(*this);
    m_preserved_wrappables.clear();
    m_wrappers.clear();
}

void WrapperWorld::register_preserved_wrappable(GCAllocatedWrappable& wrappable)
{
    VERIFY(!is_main_world());
    VERIFY(!is_detached());
    m_preserved_wrappables.set(wrappable);
}

GC::Ptr<PlatformObject> WrapperWorld::wrapper_for(Wrappable const& wrappable, JS::Realm& realm) const
{
    if (is_main_world()) {
        if (auto wrapper = wrappable.cached_main_world_wrapper(*this))
            return wrapper;

        if (auto* wrapper = m_wrappers.get(wrappable)) {
            VERIFY(&host_defined_wrapper_world(wrapper->realm()) == this);
            return const_cast<PlatformObject*>(wrapper);
        }
        return nullptr;
    }

    if (auto* wrapper = m_wrappers.get(wrappable)) {
        VERIFY(&wrapper->realm() == &realm);
        return const_cast<PlatformObject*>(wrapper);
    }
    return nullptr;
}

void WrapperWorld::set_wrapper(Wrappable& wrappable, PlatformObject& wrapper)
{
    VERIFY(!is_detached());
    verify_cache_entry(*this, wrappable, wrapper);

    if (is_main_world()) {
        if (auto* existing = m_wrappers.get(wrappable)) {
            VERIFY(existing == &wrapper);
            return;
        }

        m_wrappers.set(wrappable, wrapper);
        wrappable.set_cached_main_world_wrapper(wrapper);
        return;
    }

    if (auto* existing = m_wrappers.get(wrappable)) {
        VERIFY(existing == &wrapper);
        VERIFY(&existing->realm() == &wrapper.realm());
        return;
    }
    m_wrappers.set(wrappable, wrapper);
}

void WrapperWorld::clear_wrapper(Wrappable& wrappable, PlatformObject const& wrapper)
{
    verify_cache_entry(*this, wrappable, wrapper);

    if (is_main_world()) {
        if (auto* existing = m_wrappers.get(wrappable)) {
            if (existing == &wrapper)
                m_wrappers.remove(wrappable);
        }
        wrappable.clear_cached_main_world_wrapper(wrapper);
        return;
    }

    if (auto* existing = m_wrappers.get(wrappable)) {
        VERIFY(existing == &wrapper);
        m_wrappers.remove(wrappable);
    }
}

WrapperWorld& host_defined_wrapper_world(JS::Realm& realm)
{
    return *static_cast<Bindings::HostDefined&>(*realm.host_defined()).wrapper_world;
}

WrapperWorld const& host_defined_wrapper_world(JS::Realm const& realm)
{
    return *static_cast<Bindings::HostDefined const&>(*realm.host_defined()).wrapper_world;
}

}
