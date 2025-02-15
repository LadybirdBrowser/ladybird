/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class PreferenceActor final : public Actor {
public:
    static constexpr auto base_name = "preference"sv;

    static NonnullRefPtr<PreferenceActor> create(DevToolsServer&, ByteString name);
    virtual ~PreferenceActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

private:
    PreferenceActor(DevToolsServer&, ByteString name);
};

}
