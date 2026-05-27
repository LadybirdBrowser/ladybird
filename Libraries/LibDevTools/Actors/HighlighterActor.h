/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>
#include <LibWeb/Forward.h>

namespace DevTools {

class DEVTOOLS_API HighlighterActor final : public Actor {
public:
    static constexpr auto base_name = "highlighter"sv;

    static NonnullRefPtr<HighlighterActor> create(DevToolsServer&, String name, WeakPtr<InspectorActor>, String type_name);
    virtual ~HighlighterActor() override;

    JsonValue serialize_highlighter() const;

private:
    HighlighterActor(DevToolsServer&, String name, WeakPtr<InspectorActor>, String type_name);

    virtual void handle_message(Message const&) override;

    void clear_current_highlight();

    WeakPtr<InspectorActor> m_inspector;
    String m_type_name;
    Optional<Web::UniqueNodeID> m_highlighted_flexbox_node_id;
    Optional<Web::UniqueNodeID> m_highlighted_grid_node_id;
    bool m_is_highlighting_dom_node { false };
};

}
