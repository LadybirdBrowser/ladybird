/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Fetch/Engine/BodyPresentation.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Engine/StartFetchInput.h>

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

// The answer to a manual-redirect delivery's continuation, given exactly once.
struct ResolveRedirect {
    struct Follow { };
    struct AcceptAsFinal { };
    struct Abandon { };
    using Resolution = Variant<Follow, AcceptAsFinal, Abandon>;

    DeliveryId delivery_id;
    ContinuationId continuation_id;
    Resolution resolution;
};

// Only ever complete: all bytes present, or a network error with no body.
struct CompletedPreloadResponse {
    struct Completed {
        Delivered<Core::ImmutableBytes> body;
    };
    struct Failure {
        NullBody body;
    };

    Variant<Completed, Failure> outcome;
};

struct PreloadResponseResolved {
    FetchId fetch_id;
    CompletedPreloadResponse response;
};

struct TransportControl {
    struct ResumeBodyDeliveryUpTo {
        u64 byte_count { 0 };
    };
    struct PauseBodyDelivery { };
    struct StopTransport { };
    using Operation = Variant<ResumeBodyDeliveryUpTo, PauseBodyDelivery, StopTransport>;

    RequestId request_id;
    TransportEpoch transport_epoch;
    Operation operation;
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
    Command::AdoptNetworkTransport>;

}
