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
#include <LibWeb/Fetch/Engine/RequestEnd.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Fetch::Engine::Event {

struct ResponseDelivery {
    DeliveryId delivery_id;
    LocalBodyPresentation body;
    FetchTimingSeed timing_seed;
};

// Sent once per delivery after its last byte, whether or not the transport has completed.
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

struct RequestTransmissionEnded {
    TransmissionId transmission_id;
    TransmissionGeneration generation;
    RequestEnd request_end;
};

}

namespace Web::Fetch::Engine {

using EngineEvent = Variant<
    Event::ResponseDelivery,
    Event::EndOfBody,
    Event::ConsumeBodyResult,
    Event::BodyChannelReadable,
    Event::DeliveryEnd,
    Event::RequestTransmissionStarted,
    Event::RequestTransmissionEnded>;

}
