/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDevTools/Actor.h>
#include <LibDevTools/Actors/NodeActor.h>
#include <LibDevTools/Forward.h>
#include <LibWeb/Forward.h>

namespace DevTools {

class DEVTOOLS_API AccessibilityNodeActor final : public Actor {
public:
    static constexpr auto base_name = "accessibility-node"sv;

    static NonnullRefPtr<AccessibilityNodeActor> create(DevToolsServer&, String name, NodeIdentifier, WeakPtr<AccessibilityWalkerActor>);
    virtual ~AccessibilityNodeActor() override;

    NodeIdentifier const& node_identifier() const { return m_node_identifier; }
    WeakPtr<AccessibilityWalkerActor> const& walker() const { return m_walker; }

private:
    AccessibilityNodeActor(DevToolsServer&, String name, NodeIdentifier, WeakPtr<AccessibilityWalkerActor>);

    virtual void handle_message(Message const&) override;

    NodeIdentifier m_node_identifier;
    WeakPtr<AccessibilityWalkerActor> m_walker;
};

}
