/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ThreadID.h>
#include <AK/WeakPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/EngineEventSink.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerToken.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

// Receives a fetch's events on the agent that started the fetch, without keeping the fetch's controller alive.
class WEB_API FetchEventSink final : public Engine::EngineEventSink {
public:
    explicit FetchEventSink(Infrastructure::FetchController const&);

    virtual void handle_event(Engine::EngineEvent) override;

private:
    [[nodiscard]] GC::Ref<Infrastructure::FetchController> controller() const;

    Infrastructure::FetchControllerToken m_controller_token;
    WeakPtr<Infrastructure::FetchControllerRegistry> m_controller_registry;
    AK::ThreadID m_agent_thread_id { AK::ThreadID::current() };
};

}
