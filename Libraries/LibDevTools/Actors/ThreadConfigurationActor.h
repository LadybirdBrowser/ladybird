/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class ThreadConfigurationActor final : public Actor {
public:
    static constexpr auto base_name = "thread-configuration"sv;

    static NonnullRefPtr<ThreadConfigurationActor> create(DevToolsServer&, String name);
    virtual ~ThreadConfigurationActor() override;

    JsonObject serialize_configuration() const;

private:
    ThreadConfigurationActor(DevToolsServer&, String name);

    virtual void handle_message(Message const&) override;
};

}
