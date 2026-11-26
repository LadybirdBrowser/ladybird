/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Variant.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>
#include <LibWeb/Fetch/Engine/StartFetchInput.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::Fetch::Engine::Command {

struct StartFetch {
    StartFetchInput input;
};

struct Abort {
    FetchId fetch_id;
};

struct Terminate {
    FetchId fetch_id;
};

struct Stop {
    FetchId fetch_id;
};

// The answer to the continuation of a manual redirect a fetch handed over, given at most once.
struct ResolveRedirect {
    // The redirect's next hop, whose in-parallel steps the engine runs while the fetch is ongoing.
    struct Follow {
        MainFetchInput next_hop;
    };
    struct AcceptAsFinal { };
    struct Abandon { };
    using Resolution = Variant<Follow, AcceptAsFinal, Abandon>;

    FetchId fetch_id;
    ContinuationId continuation_id;
    Resolution resolution;
};

// The response of the preload that the fetch's main fetch waits for, or null if the preload was cancelled.
struct PreloadResponseResolved {
    FetchId fetch_id;
    RefPtr<Infrastructure::Response> response;
};

struct TransportControl {
    struct StopTransport { };
    struct ReleaseNavigationHold { };
    struct ReleaseNavigationHoldUpTo {
        u64 byte_count { 0 };
    };
    using Operation = Variant<StopTransport, ReleaseNavigationHold, ReleaseNavigationHoldUpTo>;

    RequestId request_id;
    TransportEpoch transport_epoch;
    Operation operation;
};

// The transport of a navigation body the agent adopted by its transfer lease, whose controls the engine applies from
// then on.
struct NavigationBodyAdopted {
    NonnullRefPtr<NetworkTransportHandle> handle;
    NonnullRefPtr<FetchBodyDeliveryGate> delivery_gate;
};

struct AdoptNetworkTransport {
    IPC::TransportHandle handle;
};

}

namespace Web::Fetch::Engine {

using EngineCommand = Variant<
    Command::StartFetch,
    Command::Abort,
    Command::Terminate,
    Command::Stop,
    Command::ResolveRedirect,
    Command::PreloadResponseResolved,
    Command::TransportControl,
    Command::NavigationBodyAdopted,
    Command::AdoptNetworkTransport>;

}
