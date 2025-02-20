/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class InspectorActor final : public Actor {
public:
    static constexpr auto base_name = "inspector"sv;

    static NonnullRefPtr<InspectorActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~InspectorActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

private:
    InspectorActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    void received_dom_tree(JsonObject, BlockToken);

    WeakPtr<TabActor> m_tab;
    WeakPtr<PageStyleActor> m_page_style;
    WeakPtr<HighlighterActor> m_highlighter;
};

}
