/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Variant.h>
#include <LibRequests/ALPNHttpVersion.h>
#include <LibRequests/NetworkError.h>
#include <LibWeb/Fetch/Engine/Ids.h>

namespace Web::Fetch::Engine {

// One arm per way a request can end.
struct RequestEnd {
    struct NetworkSuccess {
        AttemptId attempt_id;
        u64 encoded_body_size { 0 };
        Requests::ALPNHttpVersion alpn { Requests::ALPNHttpVersion::None };
    };
    struct LocalSuccess {
        u64 encoded_body_size { 0 };
    };
    struct AttemptFailedBeforeHeaders {
        AttemptId attempt_id;
        Requests::NetworkError error;
    };
    struct CommittedAttemptFailed {
        AttemptId attempt_id;
        Requests::NetworkError error;
        u64 encoded_body_size { 0 };
    };
    struct LocalFailed {
        Requests::NetworkError error;
        u64 encoded_body_size { 0 };
    };
    struct FailedBeforeTransport {
        Requests::NetworkError error;
    };

    using Outcome = Variant<NetworkSuccess, LocalSuccess, AttemptFailedBeforeHeaders, CommittedAttemptFailed, LocalFailed, FailedBeforeTransport>;

    RequestId request_id;
    TransportEpoch transport_epoch;
    MonotonicTime transport_complete_at;
    Outcome outcome;
};

}
