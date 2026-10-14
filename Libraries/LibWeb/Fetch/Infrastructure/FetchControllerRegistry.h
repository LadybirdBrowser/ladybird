/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Weakable.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerToken.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// The fetch controllers of one agent, by token. It keeps none of them alive.
class WEB_API FetchControllerRegistry : public Weakable<FetchControllerRegistry> {
    AK_MAKE_NONCOPYABLE(FetchControllerRegistry);
    AK_MAKE_NONMOVABLE(FetchControllerRegistry);

public:
    FetchControllerRegistry();

    [[nodiscard]] FetchControllerToken add(Badge<FetchController>, FetchController&);
    void remove(Badge<FetchController>, FetchControllerToken);

    [[nodiscard]] GC::Ptr<FetchController> find(FetchControllerToken) const;

private:
    u64 m_id { 0 };
    HashMap<u64, GC::RawPtr<FetchController>> m_controllers;
};

}
