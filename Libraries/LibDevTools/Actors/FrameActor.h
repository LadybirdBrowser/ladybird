/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibDevTools/Actor.h>
#include <LibWebView/Forward.h>

namespace DevTools {

class FrameActor final : public Actor {
public:
    static constexpr auto base_name = "frame"sv;

    static NonnullRefPtr<FrameActor> create(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<CSSPropertiesActor>, WeakPtr<ConsoleActor>, WeakPtr<InspectorActor>, WeakPtr<ThreadActor>);
    virtual ~FrameActor() override;

    void send_frame_update_message();

    JsonObject serialize_target() const;

private:
    FrameActor(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<CSSPropertiesActor>, WeakPtr<ConsoleActor>, WeakPtr<InspectorActor>, WeakPtr<ThreadActor>);

    virtual void handle_message(Message const&) override;

    void console_message_available(i32 message_index);
    void console_messages_received(i32 start_index, Vector<WebView::ConsoleOutput>);
    void request_console_messages();

    WeakPtr<TabActor> m_tab;

    WeakPtr<CSSPropertiesActor> m_css_properties;
    WeakPtr<ConsoleActor> m_console;
    WeakPtr<InspectorActor> m_inspector;
    WeakPtr<ThreadActor> m_thread;

    i32 m_highest_notified_message_index { -1 };
    i32 m_highest_received_message_index { -1 };
    bool m_waiting_for_messages { false };
};

}
