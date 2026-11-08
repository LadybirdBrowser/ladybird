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
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerToken.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// The fetch controllers of one agent, by token. Nothing in it keeps them alive but the main fetch steps it defers.
class WEB_API FetchControllerRegistry : public Weakable<FetchControllerRegistry> {
    AK_MAKE_NONCOPYABLE(FetchControllerRegistry);
    AK_MAKE_NONMOVABLE(FetchControllerRegistry);

public:
    FetchControllerRegistry();

    [[nodiscard]] FetchControllerToken add(Badge<FetchController>, FetchController&);
    void remove(Badge<FetchController>, FetchControllerToken);

    [[nodiscard]] GC::Ptr<FetchController> find(FetchControllerToken) const;

    [[nodiscard]] Vector<GC::Root<FetchController>> controllers() const;

    // Keeps main fetch's in-parallel steps alive while they wait to be taken.
    [[nodiscard]] Engine::FetchId defer_main_fetch_steps(GC::Ref<GC::Function<void()>> steps);
    [[nodiscard]] GC::Root<GC::Function<void()>> take_deferred_main_fetch_steps(Engine::FetchId);

private:
    u64 m_id { 0 };
    HashMap<u64, GC::RawPtr<FetchController>> m_controllers;
    HashMap<Engine::FetchId, GC::Root<GC::Function<void()>>> m_deferred_main_fetch_steps;
};

}
