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
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/RequestPriority.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::Fetch::Engine::Event {

// What happened to one of a fetch's RequestServer requests, as DevTools reads it.
struct NetworkActivity {
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

}
