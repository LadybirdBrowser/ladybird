/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibWebView/DOMNodeProperties.h>

namespace DevTools {

class PageStyleActor final : public Actor {
public:
    static constexpr auto base_name = "page-style"sv;

    static NonnullRefPtr<PageStyleActor> create(DevToolsServer&, String name, WeakPtr<InspectorActor>);
    virtual ~PageStyleActor() override;

    JsonValue serialize_style() const;

private:
    PageStyleActor(DevToolsServer&, String name, WeakPtr<InspectorActor>);

    virtual void handle_message(Message const&) override;

    void inspect_dom_node(Message const&, WebView::DOMNodeProperties::Type);
    void received_dom_node_properties(WebView::DOMNodeProperties const&);

    WeakPtr<InspectorActor> m_inspector;

    Vector<Message, 1> m_pending_inspect_requests;
};

}
