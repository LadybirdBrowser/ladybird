/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Fetch/Infrastructure/ConnectionTimingInfo.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Fetch::Engine {

// The timing fields settled at final response handover.
struct FetchTimingSeed {
    HighResolutionTime::DOMHighResTimeStamp start_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp redirect_start_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp redirect_end_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp post_redirect_start_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp final_service_worker_start_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp final_network_request_start_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp first_interim_network_response_start_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp final_network_response_start_time { 0 };
    Optional<Infrastructure::ConnectionTimingInfo> final_connection_timing_info;
    Vector<String> server_timing_headers;
    bool render_blocking { false };
};

}
