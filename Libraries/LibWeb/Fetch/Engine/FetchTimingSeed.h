/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Fetch/Infrastructure/ConnectionTimingInfo.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Fetch::Engine {

// The timing fields a response handover gives the agent that started the fetch.
struct FetchTimingSeed {
    HighResolutionTime::DOMHighResTimeStamp start_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp redirect_start_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp redirect_end_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp post_redirect_start_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp final_service_worker_start_time { 0 };
    Vector<String> server_timing_headers;
    bool render_blocking { false };
};

// The timing fields a load of a fetch's request settles once it has finished.
struct FetchNetworkPhases {
    Infrastructure::ConnectionTimingInfo final_connection_timing_info;
    HighResolutionTime::DOMHighResTimeStamp final_network_request_start_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp final_network_response_start_time { 0 };
};

}
