/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class RootActor final : public Actor {
public:
    static constexpr auto base_name = "root"sv;

    static NonnullRefPtr<RootActor> create(DevToolsServer&, ByteString name);
    virtual ~RootActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

private:
    RootActor(DevToolsServer&, ByteString name);
};

}
