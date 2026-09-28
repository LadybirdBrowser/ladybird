/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWebCommon/HTML/SerializationRecords.h>
#include <LibWebCommon/HTML/WorkerAgentForward.h>
#include <LibWebCommon/HTML/WorkerTypes.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>

namespace Web::HTML {

struct WEBCOMMON_API WorkerAgentStartRequest {
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
    // The rendering rate of the spawning page, used to pace rendering updates in worker event
    // loops, which have no display connection of their own.
    double maximum_frames_per_second { 60.0 };
    WorkerAgentOwnerToken owner_token { 0 };
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::WorkerAgentStartRequest const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::WorkerAgentStartRequest> decode(Decoder&);

}
