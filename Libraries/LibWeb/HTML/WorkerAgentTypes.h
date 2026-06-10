/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WorkerAgentForward.h>
#include <LibWeb/HTML/WorkerTypes.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::HTML {

using WorkerOptions = Bindings::WorkerOptions;

struct WEB_API WorkerAgentStartRequest {
    URL::URL url;
    AgentType agent_type { AgentType::DedicatedWorker };
    WorkerType type { WorkerType::Classic };
    RequestCredentials credentials { RequestCredentials::SameOrigin };
    String name;
    // FIXME: We don't implement SharedWorkerOptions/extendedLifetime yet.
    bool extended_lifetime { false };
    TransferDataEncoder outside_port;
    SerializedEnvironmentSettingsObject outside_settings;
    StorageAPI::StorageKey storage_key;
    bool caller_is_secure_context { false };
    WorkerAgentOwnerToken owner_token { 0 };
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::WorkerAgentStartRequest const&);

template<>
WEB_API ErrorOr<Web::HTML::WorkerAgentStartRequest> decode(Decoder&);

}
