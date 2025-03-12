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

    JsonValue serialize_style() const;

private:
    PageStyleActor(DevToolsServer&, String name, WeakPtr<InspectorActor>);

    virtual void handle_message(Message const&) override;

    template<typename Callback>
    void inspect_dom_node(StringView node_actor, Callback&&);

    WeakPtr<InspectorActor> m_inspector;
};

}
