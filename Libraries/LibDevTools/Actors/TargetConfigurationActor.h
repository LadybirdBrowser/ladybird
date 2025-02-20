/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class TargetConfigurationActor final : public Actor {
public:
    static constexpr auto base_name = "target-configuration"sv;

    static NonnullRefPtr<TargetConfigurationActor> create(DevToolsServer&, String name);
    virtual ~TargetConfigurationActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

    JsonObject serialize_configuration() const;

private:
    TargetConfigurationActor(DevToolsServer&, String name);
};

}
