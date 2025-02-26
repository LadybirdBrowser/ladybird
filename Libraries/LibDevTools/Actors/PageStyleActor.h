/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

struct DOMNodeProperties {
    JsonObject computed_style;
    JsonObject node_box_sizing;
};

class PageStyleActor final : public Actor {
public:
    static constexpr auto base_name = "page-style"sv;

    static NonnullRefPtr<PageStyleActor> create(DevToolsServer&, String name, WeakPtr<InspectorActor>);
    virtual ~PageStyleActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;
    JsonValue serialize_style() const;

private:
    PageStyleActor(DevToolsServer&, String name, WeakPtr<InspectorActor>);

    template<typename Callback>
    void inspect_dom_node(StringView node_actor, Callback&&);

    void received_layout(JsonObject const& computed_style, JsonObject const& node_box_sizing, BlockToken);
    void received_computed_style(JsonObject const& computed_style, BlockToken);

    WeakPtr<InspectorActor> m_inspector;
};

}
