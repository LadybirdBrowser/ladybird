/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class PageStyleActor final : public Actor {
public:
    static constexpr auto base_name = "page-style"sv;

    static NonnullRefPtr<PageStyleActor> create(DevToolsServer&, String name);
    virtual ~PageStyleActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;
    JsonValue serialize_style() const;

private:
    PageStyleActor(DevToolsServer&, String name);
};

}
