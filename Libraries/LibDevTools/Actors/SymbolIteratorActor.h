/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class SymbolIteratorActor final : public Actor {
public:
    static constexpr auto base_name = "symbolIterator"sv;

    static NonnullRefPtr<SymbolIteratorActor> create(DevToolsServer&, String name, WeakPtr<ThreadActor>);
    virtual ~SymbolIteratorActor() override;

    JsonObject form() const;

private:
    SymbolIteratorActor(DevToolsServer&, String name, WeakPtr<ThreadActor>);

    virtual void handle_message(Message const&) override;

    WeakPtr<ThreadActor> m_thread;
};

}
