/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class WatcherActor final : public Actor {
public:
    static constexpr auto base_name = "watcher"sv;

    static NonnullRefPtr<WatcherActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~WatcherActor() override;

    JsonObject serialize_description() const;

private:
    WatcherActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    WeakPtr<TabActor> m_tab;
    WeakPtr<Actor> m_target;
    WeakPtr<TargetConfigurationActor> m_target_configuration;
    WeakPtr<ThreadConfigurationActor> m_thread_configuration;
};

}
