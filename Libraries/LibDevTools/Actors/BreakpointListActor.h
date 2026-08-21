/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>
#include <LibWebView/Debugger.h>

namespace DevTools {

class DEVTOOLS_API BreakpointListActor final : public Actor {
public:
    static constexpr auto base_name = "breakpoint-list"sv;

    static NonnullRefPtr<BreakpointListActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~BreakpointListActor() override;

    void reapply_breakpoints();

private:
    struct Breakpoint {
        WebView::DebuggerBreakpointLocation location;
        WebView::DebuggerBreakpointOptions options;
    };

    BreakpointListActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    Optional<WebView::DebuggerBreakpointLocation> breakpoint_location(Message const&);
    void send_breakpoint_error(Message const&, Error const&);
    void remember_breakpoint(WebView::DebuggerBreakpointLocation, WebView::DebuggerBreakpointOptions);
    void forget_breakpoint(WebView::DebuggerBreakpointLocation const&);

    WeakPtr<TabActor> m_tab;
    Vector<Breakpoint> m_breakpoints;
};

}
