/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>

namespace Web::Fetch::Infrastructure {

static Atomic<u64> s_next_registry_id { 1 };
static Atomic<u64> s_next_serial { 1 };

FetchControllerRegistry::FetchControllerRegistry()
    : m_id(s_next_registry_id.fetch_add(1))
{
}

FetchControllerToken FetchControllerRegistry::add(Badge<FetchController>, FetchController& controller)
{
    FetchControllerToken token { m_id, s_next_serial.fetch_add(1) };
    m_controllers.set(token.m_serial, &controller);
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
    return m_controllers.get(token.m_serial).value_or(nullptr);
}

}
