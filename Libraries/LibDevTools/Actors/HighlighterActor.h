/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class HighlighterActor final : public Actor {
public:
    static constexpr auto base_name = "highlighter"sv;

    static NonnullRefPtr<HighlighterActor> create(DevToolsServer&, ByteString name);
    virtual ~HighlighterActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;
    JsonValue serialize_highlighter() const;

private:
    HighlighterActor(DevToolsServer&, ByteString name);
};

}
