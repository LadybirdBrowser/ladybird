/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>
#include <LibWebView/Debugger.h>

namespace DevTools {

class DEVTOOLS_API DebuggerFrameActor final : public Actor {
public:
    static constexpr auto base_name = "debugger-frame"sv;

    static NonnullRefPtr<DebuggerFrameActor> create(DevToolsServer&, String name, WeakPtr<ThreadActor>, WebView::DebuggerFrame, String source_actor, bool oldest);
    virtual ~DebuggerFrameActor() override;

    u64 frame_id() const { return m_frame.id; }
    JsonObject serialize_frame() const;

private:
    DebuggerFrameActor(DevToolsServer&, String name, WeakPtr<ThreadActor>, WebView::DebuggerFrame, String source_actor, bool oldest);

    virtual void handle_message(Message const&) override;

    WeakPtr<ThreadActor> m_thread;
    WebView::DebuggerFrame m_frame;
    String m_source_actor;
    bool m_oldest { false };
};

}
