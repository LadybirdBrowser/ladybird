/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerToken.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// Tracks one agent's controllers by token and roots each started fetch's controller until the fetch ends.
class WEB_API FetchControllerRegistry : public Weakable<FetchControllerRegistry> {
    AK_MAKE_NONCOPYABLE(FetchControllerRegistry);
    AK_MAKE_NONMOVABLE(FetchControllerRegistry);

public:
    FetchControllerRegistry();

    [[nodiscard]] FetchControllerToken add(Badge<FetchController>, FetchController&);
    void remove(Badge<FetchController>, FetchControllerToken);

    [[nodiscard]] GC::Ptr<FetchController> find(FetchControllerToken) const;

    [[nodiscard]] Vector<GC::Root<FetchController>> controllers() const;

    // A fetch id above every one given before.
    [[nodiscard]] static Engine::FetchId next_fetch_id();

    // Keeps the controller of a fetch that has started alive until the fetch is released.
    void root_fetch(Engine::FetchId, GC::Ref<FetchController>);
    // Does nothing for a fetch it does not root.
    void release_fetch(Engine::FetchId);
    [[nodiscard]] bool roots(FetchControllerToken) const;

private:
    u64 m_id { 0 };
    HashMap<u64, GC::Weak<FetchController>> m_controllers;

    HashMap<Engine::FetchId, GC::Root<FetchController>> m_rooted_fetches;
};

}
