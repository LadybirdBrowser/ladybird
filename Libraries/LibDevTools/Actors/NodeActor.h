/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class NodeActor final : public Actor {
public:
    static constexpr auto base_name = "node"sv;

    static NonnullRefPtr<NodeActor> create(DevToolsServer&, String name, WeakPtr<WalkerActor>);
    virtual ~NodeActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

private:
    NodeActor(DevToolsServer&, String name, WeakPtr<WalkerActor>);

    WeakPtr<WalkerActor> m_walker;
};

}
