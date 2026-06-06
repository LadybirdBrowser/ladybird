/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    if (world.is_main_world())
        VERIFY(&wrapper.realm() == &wrappable.realm());
    VERIFY(wrappable_impl_from(&wrapper) == &wrappable);
}

WrapperWorld::WrapperWorld(Type type)
    : m_type(type)
{
}

WrapperWorld::~WrapperWorld() = default;

GC::Ptr<PlatformObject> WrapperWorld::wrapper_for(Wrappable const& wrappable) const
{
    if (is_main_world())
        return wrappable.cached_main_world_wrapper();

    auto it = m_wrappers.find(&wrappable);
    if (it == m_wrappers.end())
        return nullptr;
    return it->value.ptr();
}

void WrapperWorld::set_wrapper(Wrappable& wrappable, PlatformObject& wrapper)
{
    verify_cache_entry(*this, wrappable, wrapper);

    if (is_main_world()) {
        wrappable.set_cached_main_world_wrapper(wrapper);
        return;
    }

    if (auto it = m_wrappers.find(&wrappable); it != m_wrappers.end()) {
        if (auto existing_wrapper = it->value.ptr())
            VERIFY(existing_wrapper == &wrapper);
    }
    m_wrappers.set(&wrappable, wrapper);
}

void WrapperWorld::clear_wrapper(Wrappable& wrappable, PlatformObject const& wrapper)
{
    verify_cache_entry(*this, wrappable, wrapper);

    if (is_main_world()) {
        wrappable.clear_cached_main_world_wrapper(wrapper);
        return;
    }

    auto it = m_wrappers.find(&wrappable);
    if (it != m_wrappers.end() && it->value.ptr() == &wrapper)
        m_wrappers.remove(it);
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
