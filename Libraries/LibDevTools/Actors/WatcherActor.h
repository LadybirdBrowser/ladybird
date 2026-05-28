/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class DEVTOOLS_API WatcherActor final : public Actor {
public:
    static constexpr auto base_name = "watcher"sv;

    static NonnullRefPtr<WatcherActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~WatcherActor() override;

    JsonObject serialize_description() const;
    void send_frame_target_available_message();
    void switch_frame_target(FrameActor&, String const& url, String const& title);

private:
    WatcherActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    FrameActor& create_frame_target();
    void send_frame_target_available_message(FrameActor&);
    void send_frame_target_destroyed_message(FrameActor&);

    WeakPtr<TabActor> m_tab;
    WeakPtr<FrameActor> m_target;
    WeakPtr<TargetConfigurationActor> m_target_configuration;
    WeakPtr<ThreadConfigurationActor> m_thread_configuration;
    WeakPtr<NetworkParentActor> m_network_parent;
    bool m_is_watching_frame_targets { false };
};

}
