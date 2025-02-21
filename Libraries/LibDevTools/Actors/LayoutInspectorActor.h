/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class LayoutInspectorActor final : public Actor {
public:
    static constexpr auto base_name = "layout-inspector"sv;

    static NonnullRefPtr<LayoutInspectorActor> create(DevToolsServer&, String name);
    virtual ~LayoutInspectorActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

private:
    LayoutInspectorActor(DevToolsServer&, String name);
};

}
