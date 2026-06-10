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
    if (world.is_main_world())
        VERIFY(host_defined_wrapper_world(wrapper.realm()).is_main_world());
    else
        VERIFY(&host_defined_wrapper_world(wrapper.realm()) == &world);
    VERIFY(wrappable_impl_from(&wrapper) == &wrappable);
}

WrapperWorld::WrapperWorld(Type type)
    : m_type(type)
{
}

WrapperWorld::~WrapperWorld() = default;

void WrapperWorld::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

GC::Ptr<PlatformObject> WrapperWorld::wrapper_for(Wrappable const& wrappable, JS::Realm& realm) const
{
    if (is_main_world())
        return wrappable.cached_main_world_wrapper();

    for (auto const& entry : m_wrappers) {
        if (entry.wrappable.ptr() == &wrappable && entry.wrapper && &entry.wrapper->realm() == &realm)
            return entry.wrapper.ptr();
    }
    return nullptr;
}

void WrapperWorld::set_wrapper(Wrappable& wrappable, PlatformObject& wrapper)
{
    verify_cache_entry(*this, wrappable, wrapper);

    if (is_main_world()) {
        wrappable.set_cached_main_world_wrapper(wrapper);
        return;
    }

    m_wrappers.remove_all_matching([](auto const& entry) {
        return !entry.wrappable || !entry.wrapper;
    });

    for (auto const& entry : m_wrappers) {
        if (entry.wrappable.ptr() == &wrappable && &entry.wrapper->realm() == &wrapper.realm()) {
            VERIFY(entry.wrapper.ptr() == &wrapper);
            return;
        }
    }
    m_wrappers.append({ &wrappable, wrapper });
}

void WrapperWorld::clear_wrapper(Wrappable& wrappable, PlatformObject const& wrapper)
{
    verify_cache_entry(*this, wrappable, wrapper);

    if (is_main_world()) {
        wrappable.clear_cached_main_world_wrapper(wrapper);
        return;
    }

    m_wrappers.remove_all_matching([&](auto const& entry) {
        return !entry.wrappable || !entry.wrapper || (entry.wrappable.ptr() == &wrappable && entry.wrapper.ptr() == &wrapper);
    });
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
