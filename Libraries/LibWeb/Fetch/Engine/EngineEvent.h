/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibWeb/Fetch/Engine/BodyPresentation.h>
#include <LibWeb/Fetch/Engine/FetchTimingSeed.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Engine/NetworkActivity.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Fetch::Engine::Event {

struct ResponseDelivery {
    DeliveryId delivery_id;
    LocalBodyPresentation body;
    FetchTimingSeed timing_seed;
};

// Sent only for a body still arriving at its response's handover, once the network completes it.
struct EndOfBody {
    struct FinalSizes {
        u64 encoded_body_size { 0 };
        u64 decoded_body_size { 0 };
    };

    DeliveryId delivery_id;
    FinalSizes final_sizes;
    HighResolutionTime::DOMHighResTimeStamp end_time { 0 };
};

struct ConsumeBodyResult {
    struct Null { };
    struct Bytes {
        Core::ImmutableBytes bytes;
    };
    struct Failure { };
    using Outcome = Variant<Null, Bytes, Failure>;

    DeliveryId delivery_id;
    Outcome outcome;
};

struct BodyChannelReadable {
    DeliveryId delivery_id;
    WakeTicket ticket { 0 };
};

// A delivery's end, committed exactly once by the engine.
struct DeliveryEnd {
    struct Released { };
    struct Revoked { };
    using Outcome = Variant<Released, Revoked>;

    DeliveryId delivery_id;
    Outcome outcome;
};

struct RequestTransmissionStarted {
    TransmissionId transmission_id;
    TransmissionGeneration generation;
};

// A timing update: the network phases a load of the fetch's request settled as it finished. Only a load that completes
// sends one.
struct RequestTransmissionEnded {
    FetchNetworkPhases network_phases;
};

// The length of one more chunk of a request's body.
struct RequestBodyProgress {
    u64 length { 0 };
};

// The end of a request's body after its last chunk, or the end of a null body.
struct RequestBodyEndOfBody {
    bool body_is_null { false };
};

// Asks the fetch's agent to read a request's body from its stream, the only place its bytes are.
struct ReadRequestBodyStream { };

// A request's body is sent from its source.
struct RequestBodySentFromSource { };

// The fetch's last event: its engine will send nothing more for it.
struct FetchEnded { };

}

namespace Web::Fetch::Engine {

using EngineEvent = Variant<
    Event::ResponseDelivery,
    Event::EndOfBody,
    Event::ConsumeBodyResult,
    Event::BodyChannelReadable,
    Event::DeliveryEnd,
    Event::RequestTransmissionStarted,
    Event::RequestTransmissionEnded,
    Event::RequestBodyProgress,
    Event::RequestBodyEndOfBody,
    Event::ReadRequestBodyStream,
    Event::RequestBodySentFromSource,
    Event::NetworkActivity,
    Event::FetchEnded>;

}
