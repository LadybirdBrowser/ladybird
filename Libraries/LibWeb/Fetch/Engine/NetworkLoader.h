/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <LibCore/ImmutableBytes.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/CacheState.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Engine/NetworkLoadRequest.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>

namespace Web::Fetch::Engine {

// Loads the requests of one fetch's network step.
class WEB_API NetworkLoader : public RefCounted<NetworkLoader> {
public:
    // A callback's transport is that of the RequestServer request the load started, or null if it started none.
    using OnHeadersReceived = Function<void(NetworkTransportHandle*, HTTP::HeaderList const& response_headers, Optional<u32> status_code, Optional<String> const& reason_phrase, Requests::CacheState cache_state)>;
    using OnDataReceived = Function<void(NetworkTransportHandle*, Requests::ResponseData data)>;
    using OnCachedBodyAvailable = Function<void(Core::ImmutableBytes data)>;

    // What RequestServer reported at the end of the request a load started.
    struct NetworkRequestEnd {
        RequestId request_id;
        u64 total_size { 0 };
        Optional<Requests::NetworkError> network_error;
    };
    using OnComplete = Function<void(bool success, Requests::RequestTimingInfo const& timing_info, Optional<StringView> error_message, Optional<NetworkRequestEnd> const& network_request_end)>;

    virtual ~NetworkLoader() = default;

    [[nodiscard]] virtual bool can_load() const = 0;

    virtual RefPtr<NetworkTransportHandle> load(NetworkLoadRequest&, OnHeadersReceived, OnDataReceived, OnCachedBodyAvailable, OnComplete, Requests::RequestClient::TransferLease) = 0;
};

}
