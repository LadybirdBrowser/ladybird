/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDevTools/Actor.h>

namespace DevTools {

class EnvironmentActor final : public Actor {
public:
    static constexpr auto base_name = "environment"sv;

    static NonnullRefPtr<EnvironmentActor> create(DevToolsServer&, String name, JsonObject form);
    virtual ~EnvironmentActor() override;

    JsonObject const& form() const { return m_form; }

private:
    EnvironmentActor(DevToolsServer&, String name, JsonObject form);

    virtual void handle_message(Message const&) override;

    JsonObject m_form;
};

}
