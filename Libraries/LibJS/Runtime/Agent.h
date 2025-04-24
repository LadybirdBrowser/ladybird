/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>

namespace JS {

// https://tc39.es/ecma262/#sec-agents
class Agent {
public:
    enum class CanBlock {
        Yes,
        No,
    };

    virtual ~Agent();

    CanBlock can_block() const { return m_can_block; }

    virtual void spin_event_loop_until(GC::Root<GC::Function<bool()>> goal_condition) = 0;

protected:
    explicit Agent(CanBlock can_block)
        : m_can_block(can_block)
    {
    }

private:
    // [[CanBlock]]
    CanBlock m_can_block { false };
};

bool agent_can_suspend(VM const&);

}
