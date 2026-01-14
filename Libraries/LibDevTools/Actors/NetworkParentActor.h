/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDevTools/Actor.h>

namespace DevTools {

class DEVTOOLS_API NetworkParentActor final : public Actor {
public:
    static constexpr auto base_name = "networkParent"sv;

    static NonnullRefPtr<NetworkParentActor> create(DevToolsServer&, String name);

private:
    NetworkParentActor(DevToolsServer&, String name);
    virtual ~NetworkParentActor() override;

    virtual void handle_message(Message const&) override;
};

}
