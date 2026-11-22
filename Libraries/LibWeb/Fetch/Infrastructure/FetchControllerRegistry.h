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
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerToken.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// The fetch controllers of one agent, by token. It keeps alive the main fetch steps it defers, and the controller of
// each started fetch until the fetch ends, except while main fetch waits for a preload.
class WEB_API FetchControllerRegistry : public Weakable<FetchControllerRegistry> {
    AK_MAKE_NONCOPYABLE(FetchControllerRegistry);
    AK_MAKE_NONMOVABLE(FetchControllerRegistry);

public:
    using DeferredMainFetchSteps = GC::Function<void(NonnullRefPtr<Engine::FetchEngineRecord>)>;

    FetchControllerRegistry();

    [[nodiscard]] FetchControllerToken add(Badge<FetchController>, FetchController&);
    void remove(Badge<FetchController>, FetchControllerToken);

    [[nodiscard]] GC::Ptr<FetchController> find(FetchControllerToken) const;

    [[nodiscard]] Vector<GC::Root<FetchController>> controllers() const;

    // Keeps main fetch's in-parallel steps alive while they wait to be taken, under a new fetch id, or under the id of
    // the fetch whose manual redirect they follow.
    [[nodiscard]] Engine::FetchId defer_main_fetch_steps(GC::Ref<DeferredMainFetchSteps> steps);
    void defer_main_fetch_steps(Engine::FetchId, GC::Ref<DeferredMainFetchSteps> steps);
    [[nodiscard]] GC::Root<DeferredMainFetchSteps> take_deferred_main_fetch_steps(Engine::FetchId);

    // Keeps the controller of a fetch that has started alive until the fetch is released, and with it the controllers of
    // the fetch's CORS preflights.
    void root_fetch(Engine::FetchId, GC::Ref<FetchController>);
    void root_cors_preflight(Engine::FetchId, GC::Ref<FetchController>);
    // Does nothing for a fetch it does not root.
    void release_fetch(Engine::FetchId);
    [[nodiscard]] bool roots(FetchControllerToken) const;

private:
    u64 m_id { 0 };
    HashMap<u64, GC::RawPtr<FetchController>> m_controllers;
    HashMap<Engine::FetchId, GC::Root<DeferredMainFetchSteps>> m_deferred_main_fetch_steps;

    struct RootedFetch {
        GC::Root<FetchController> controller;
        Vector<GC::Root<FetchController>> cors_preflight_controllers;
    };
    HashMap<Engine::FetchId, RootedFetch> m_rooted_fetches;
};

}
