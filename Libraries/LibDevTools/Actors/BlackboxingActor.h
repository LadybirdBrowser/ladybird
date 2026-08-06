/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>
#include <LibWebView/Debugger.h>

namespace DevTools {

class DEVTOOLS_API BlackboxingActor final : public Actor {
public:
    static constexpr auto base_name = "blackboxing"sv;

    static NonnullRefPtr<BlackboxingActor> create(DevToolsServer&, String name, WeakPtr<WatcherActor>);
    virtual ~BlackboxingActor() override;

    static Optional<WebView::DebuggerBlackboxRange> parse_range(JsonObject const&);

private:
    BlackboxingActor(DevToolsServer&, String name, WeakPtr<WatcherActor>);

    virtual void handle_message(Message const&) override;

    WeakPtr<WatcherActor> m_watcher;
};

}
