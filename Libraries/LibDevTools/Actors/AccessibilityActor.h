/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDevTools/Actor.h>

namespace DevTools {

class DEVTOOLS_API AccessibilityActor final : public Actor {
public:
    static constexpr auto base_name = "accessibility"sv;

    static NonnullRefPtr<AccessibilityActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~AccessibilityActor() override;

    static RefPtr<TabActor> tab_for(WeakPtr<AccessibilityActor> const&);
    static RefPtr<AccessibilityWalkerActor> walker_for(WeakPtr<AccessibilityActor> const&);

    void enable();

private:
    AccessibilityActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    void received_accessibility_tree(JsonObject& response, JsonObject accessibility_tree);

    WeakPtr<TabActor> m_tab;
    WeakPtr<AccessibilityWalkerActor> m_walker;
    bool m_enabled { false };
};

}
