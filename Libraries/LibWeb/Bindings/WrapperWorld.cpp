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

static void verify_cache_entry(WrapperWorld const& world, Wrappable const& wrappable, JS::HostObject const& wrapper)
{
    VERIFY(wrapper.shape().realm().host_defined());
    VERIFY(&host_defined_wrapper_world(wrapper.shape().realm()) == &world);
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
    m_rust_wrappers.clear();
}

void WrapperWorld::register_preserved_wrappable(GCAllocatedWrappable& wrappable)
{
    VERIFY(!is_main_world());
    VERIFY(!is_detached());
    m_preserved_wrappables.set(wrappable);
}

GC::Ptr<JS::HostObject> WrapperWorld::wrapper_for(Wrappable const& wrappable, JS::Realm& realm) const
{
    if (is_main_world()) {
        if (auto wrapper = wrappable.cached_main_world_wrapper(*this))
            return wrapper;
        return nullptr;
    }

    if (auto* wrapper = m_wrappers.get(wrappable)) {
        VERIFY(&wrapper->shape().realm() == &realm);
        return const_cast<JS::HostObject*>(wrapper);
    }
    return nullptr;
}

void WrapperWorld::set_wrapper(Wrappable& wrappable, JS::HostObject& wrapper)
{
    VERIFY(!is_detached());
    verify_cache_entry(*this, wrappable, wrapper);

    if (is_main_world()) {
        wrappable.set_cached_main_world_wrapper(wrapper);
        return;
    }

    if (auto* existing = m_wrappers.get(wrappable)) {
        VERIFY(existing == &wrapper);
        VERIFY(&existing->shape().realm() == &wrapper.shape().realm());
        return;
    }
    m_wrappers.set(wrappable, wrapper);
}

void WrapperWorld::clear_wrapper(Wrappable& wrappable, JS::HostObject const& wrapper)
{
    verify_cache_entry(*this, wrappable, wrapper);

    if (is_main_world()) {
        wrappable.clear_cached_main_world_wrapper(wrapper);
        return;
    }

    if (auto* existing = m_wrappers.get(wrappable)) {
        VERIFY(existing == &wrapper);
        m_wrappers.remove(wrappable);
    }
}

GC::Ptr<JS::HostObject> WrapperWorld::rust_wrapper_for(RustInterfaceCell const& cell, JS::Realm& realm) const
{
    VERIFY(!is_main_world());
    auto* wrapper = m_rust_wrappers.get(cell);
    if (!wrapper)
        return nullptr;
    VERIFY(&wrapper->shape().realm() == &realm);
    return const_cast<JS::HostObject*>(wrapper);
}

void WrapperWorld::set_rust_wrapper(RustInterfaceCell& cell, JS::HostObject& wrapper)
{
    VERIFY(!is_main_world());
    VERIFY(!is_detached());
    VERIFY(wrapper.shape().realm().host_defined());
    VERIFY(&host_defined_wrapper_world(wrapper.shape().realm()) == this);
    VERIFY(JS::host_data_of(wrapper).ptr() == GC::as_cell(&cell));
    // NB: The entry of a wrapper that was collected stays in the map until the map prunes it. get() does not return
    //     the wrapper of such an entry, and set() replaces it.
    VERIFY(!m_rust_wrappers.get(cell));
    m_rust_wrappers.set(cell, wrapper);
}

WrapperWorld& host_defined_wrapper_world(JS::Realm& realm)
{
    return *host_defined_of(realm).wrapper_world;
}

WrapperWorld const& host_defined_wrapper_world(JS::Realm const& realm)
{
    return *host_defined_of(realm).wrapper_world;
}

}
