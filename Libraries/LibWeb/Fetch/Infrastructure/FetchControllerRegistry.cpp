/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>

namespace Web::Fetch::Infrastructure {

static Atomic<u64> s_next_registry_id { 1 };
static Atomic<u64> s_next_serial { 1 };
static Atomic<u64> s_next_fetch_id { 1 };

FetchControllerRegistry::FetchControllerRegistry()
    : m_id(s_next_registry_id.fetch_add(1))
{
}

FetchControllerToken FetchControllerRegistry::add(Badge<FetchController>, FetchController& controller)
{
    FetchControllerToken token { m_id, s_next_serial.fetch_add(1) };
    m_controllers.set(token.m_serial, controller);
    return token;
}

void FetchControllerRegistry::remove(Badge<FetchController>, FetchControllerToken token)
{
    VERIFY(token.m_registry_id == m_id);
    m_controllers.remove(token.m_serial);
}

GC::Ptr<FetchController> FetchControllerRegistry::find(FetchControllerToken token) const
{
    VERIFY(token.m_registry_id == m_id);
    auto controller = m_controllers.get(token.m_serial);
    if (!controller.has_value())
        return nullptr;
    return controller->ptr();
}

Vector<GC::Root<FetchController>> FetchControllerRegistry::controllers() const
{
    Vector<GC::Root<FetchController>> controllers;
    controllers.ensure_capacity(m_controllers.size());
    for (auto const& entry : m_controllers) {
        if (auto controller = entry.value.ptr())
            controllers.unchecked_append(GC::make_root(*controller));
    }
    return controllers;
}

Engine::FetchId FetchControllerRegistry::next_fetch_id()
{
    return Engine::FetchId { s_next_fetch_id.fetch_add(1) };
}

void FetchControllerRegistry::root_fetch(Engine::FetchId fetch_id, GC::Ref<FetchController> controller)
{
    RootedFetch rooted_fetch { .controller = GC::make_root(controller), .cors_preflight_controllers = {} };
    auto result = m_rooted_fetches.set(fetch_id, move(rooted_fetch));
    VERIFY(result == HashSetResult::InsertedNewEntry);
}

void FetchControllerRegistry::root_cors_preflight(Engine::FetchId fetch_id, GC::Ref<FetchController> controller)
{
    auto rooted_fetch = m_rooted_fetches.find(fetch_id);
    VERIFY(rooted_fetch != m_rooted_fetches.end());
    rooted_fetch->value.cors_preflight_controllers.append(GC::make_root(controller));
}

void FetchControllerRegistry::release_fetch(Engine::FetchId fetch_id)
{
    m_rooted_fetches.remove(fetch_id);
}

bool FetchControllerRegistry::roots(FetchControllerToken token) const
{
    VERIFY(token.m_registry_id == m_id);
    for (auto const& [fetch_id, rooted_fetch] : m_rooted_fetches) {
        if (rooted_fetch.controller->token() == token)
            return true;
        for (auto const& cors_preflight_controller : rooted_fetch.cors_preflight_controllers) {
            if (cors_preflight_controller->token() == token)
                return true;
        }
    }
    return false;
}

}
