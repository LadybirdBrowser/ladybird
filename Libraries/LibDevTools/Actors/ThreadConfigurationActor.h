/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>
#include <LibWebView/Debugger.h>

namespace DevTools {

class DEVTOOLS_API ThreadConfigurationActor final : public Actor {
public:
    static constexpr auto base_name = "thread-configuration"sv;

    static NonnullRefPtr<ThreadConfigurationActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~ThreadConfigurationActor() override;

    JsonObject serialize_configuration() const;
    void reapply_configuration() const;

private:
    ThreadConfigurationActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    WeakPtr<TabActor> m_tab;
    WebView::DebuggerConfiguration m_configuration;
};

}
