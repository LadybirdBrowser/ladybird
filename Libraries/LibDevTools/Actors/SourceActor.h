/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class DEVTOOLS_API SourceActor final : public Actor {
public:
    static constexpr auto base_name = "source"sv;

    static NonnullRefPtr<SourceActor> create(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<WatcherActor>, Web::HTML::ScriptRegistry::Description);
    virtual ~SourceActor() override;

    JsonObject serialize_source() const;
    Web::HTML::ScriptRegistry::Description const& source() const { return m_source; }

private:
    SourceActor(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<WatcherActor>, Web::HTML::ScriptRegistry::Description);

    virtual void handle_message(Message const&) override;
    String source_url() const;

    WeakPtr<TabActor> m_tab;
    WeakPtr<WatcherActor> m_watcher;
    Web::HTML::ScriptRegistry::Description m_source;
};

}
