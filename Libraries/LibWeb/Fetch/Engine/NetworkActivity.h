/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/ImmutableBytes.h>
#include <LibHTTP/Header.h>
#include <LibRequests/CacheState.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Engine/FetchTimingSeed.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Engine/NetworkInterception.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/RequestPriority.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::Fetch::Engine::Event {

// What happened to one of a fetch's RequestServer requests, as DevTools reads it.
struct TransportActivity {
    struct RequestStarted {
        URL::URL url;
        ByteString method;
        Vector<HTTP::Header> headers;
        Core::ImmutableBytes body;
        Optional<String> initiator_type;
        ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };
        bool is_navigation_request { false };
        Infrastructure::RequestPriority priority { Infrastructure::RequestPriority::Auto };
    };
    struct ResponseHeaders {
        u32 status_code { 0 };
        Optional<String> reason_phrase;
        Vector<HTTP::Header> headers;
        Requests::CacheState cache_state { Requests::CacheState::NotCached };
    };
    struct ResponseBodyBytes {
        Core::ImmutableBytes bytes;
    };
    struct RequestFinished {
        u64 total_size { 0 };
        Requests::RequestTimingInfo timing_info;
        Optional<Requests::NetworkError> network_error;
    };
    using Activity = Variant<RequestStarted, ResponseHeaders, ResponseBodyBytes, RequestFinished>;

    RequestId request_id;
    Activity activity;
};

// A request, as WebDriver BiDi reports it, with the timing of the fetch it belongs to.
struct ObservedRequest {
    URL::URL url;
    ByteString method;
    Vector<HTTP::Header> headers;
    Optional<u64> body_size;
    String destination;
    Optional<String> initiator_type;
    u8 redirect_count { 0 };
    Optional<HTML::CrossProcessId> navigable_id;
    Optional<String> navigation_id;
    Optional<double> time_origin;
    FetchTimingSeed timing_seed;
    Optional<FetchNetworkPhases> network_phases;
};

struct ObservedResponse {
    Optional<URL::URL> url;
    u16 status { 0 };
    ByteString status_message;
    Vector<HTTP::Header> headers;
    // The HTTP version of the response's network transport, if it has one.
    ByteString protocol;
    bool from_cache { false };
    u64 encoded_body_size { 0 };
    u64 decoded_body_size { 0 };
};

struct FetchNetworkActivity {
    // The request reached a network phase. The fetch waits for the agent's decision if an interception is offered.
    struct BeforeRequestSent {
        Optional<ContinuationId> interception;
    };
    struct ResponseStarted {
        ObservedResponse response;
        Optional<ContinuationId> interception;
    };
    struct AuthRequired {
        ObservedResponse response;
        Optional<ContinuationId> interception;
    };

    struct RequestUpdated { };

    struct ResponseCompleted {
        ObservedResponse response;
    };

    struct Failed {
        String reason;
    };

    using Activity = Variant<BeforeRequestSent, ResponseStarted, AuthRequired, RequestUpdated, ResponseCompleted, Failed>;

    [[nodiscard]] Optional<NetworkPhase> phase() const
    {
        return activity.visit(
            [](BeforeRequestSent const&) -> Optional<NetworkPhase> { return NetworkPhase::BeforeRequestSent; },
            [](ResponseStarted const&) -> Optional<NetworkPhase> { return NetworkPhase::ResponseStarted; },
            [](AuthRequired const&) -> Optional<NetworkPhase> { return NetworkPhase::AuthRequired; },
            [](auto const&) -> Optional<NetworkPhase> { return {}; });
    }

    void offer_interception(ContinuationId id)
    {
        activity.visit(
            [&](OneOf<BeforeRequestSent, ResponseStarted, AuthRequired> auto& phase) { phase.interception = id; },
            [](auto&) { VERIFY_NOT_REACHED(); });
    }

    // The CORS preflight whose request this is, or none for the fetch's own request.
    Optional<CorsPreflightId> cors_preflight_id;
    ObservedRequest request;
    Activity activity;
};

}
