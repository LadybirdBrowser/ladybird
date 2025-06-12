/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class InspectorActor final : public Actor {
public:
    static constexpr auto base_name = "inspector"sv;

    static NonnullRefPtr<InspectorActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~InspectorActor() override;

    static RefPtr<TabActor> tab_for(WeakPtr<InspectorActor> const&);
    static RefPtr<WalkerActor> walker_for(WeakPtr<InspectorActor> const&);

private:
    InspectorActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    void received_dom_tree(JsonObject& response, JsonObject dom_tree);

    WeakPtr<TabActor> m_tab;
    WeakPtr<WalkerActor> m_walker;
    WeakPtr<PageStyleActor> m_page_style;
    HashMap<String, WeakPtr<HighlighterActor>> m_highlighters;
};

}
