/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class DeviceActor final : public Actor {
public:
    static constexpr auto base_name = "device"sv;

    static NonnullRefPtr<DeviceActor> create(DevToolsServer&, String name);
    virtual ~DeviceActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

private:
    DeviceActor(DevToolsServer&, String name);
};

}
