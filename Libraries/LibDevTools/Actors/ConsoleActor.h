/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class ConsoleActor final : public Actor {
public:
    static constexpr auto base_name = "console"sv;

    static NonnullRefPtr<ConsoleActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~ConsoleActor() override;

private:
    ConsoleActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    WeakPtr<TabActor> m_tab;

    u64 m_execution_id { 0 };
};

}
