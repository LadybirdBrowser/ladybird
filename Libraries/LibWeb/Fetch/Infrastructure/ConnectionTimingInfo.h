/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/FlyString.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#connection-timing-info
struct ConnectionTimingInfo {
    // https://fetch.spec.whatwg.org/#connection-timing-info-domain-lookup-start-time
    // domain lookup start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp domain_lookup_start_time { 0 };

    // https://fetch.spec.whatwg.org/#connection-timing-info-domain-lookup-end-time
    // domain lookup end time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp domain_lookup_end_time { 0 };

    // https://fetch.spec.whatwg.org/#connection-timing-info-connection-start-time
    // connection start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp connection_start_time { 0 };

    // https://fetch.spec.whatwg.org/#connection-timing-info-connection-end-time
    // connection end time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp connection_end_time { 0 };

    // https://fetch.spec.whatwg.org/#connection-timing-info-secure-connection-start-time
    // secure connection start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp secure_connection_start_time { 0 };

    // https://fetch.spec.whatwg.org/#connection-timing-info-alpn-negotiated-protocol
    // ALPN negotiated protocol (default the empty byte sequence)
    //     A byte sequence.
    FlyString alpn_negotiated_protocol;
};

}
