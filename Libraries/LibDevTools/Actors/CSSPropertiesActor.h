/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

struct CSSProperty {
    ByteString name;
    bool is_inherited { false };
};

class CSSPropertiesActor final : public Actor {
public:
    static constexpr auto base_name = "css-properties"sv;

    static NonnullRefPtr<CSSPropertiesActor> create(DevToolsServer&, ByteString name);
    virtual ~CSSPropertiesActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

private:
    CSSPropertiesActor(DevToolsServer&, ByteString name);
};

}
