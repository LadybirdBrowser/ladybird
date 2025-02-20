/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class FrameActor final : public Actor {
public:
    static constexpr auto base_name = "frame"sv;

    static NonnullRefPtr<FrameActor> create(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<CSSPropertiesActor>, WeakPtr<InspectorActor>, WeakPtr<ThreadActor>);
    virtual ~FrameActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;
    void send_frame_update_message();

    JsonObject serialize_target() const;

private:
    FrameActor(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<CSSPropertiesActor>, WeakPtr<InspectorActor>, WeakPtr<ThreadActor>);

    WeakPtr<TabActor> m_tab;

    WeakPtr<CSSPropertiesActor> m_css_properties;
    WeakPtr<InspectorActor> m_inspector;
    WeakPtr<ThreadActor> m_thread;
};

}
