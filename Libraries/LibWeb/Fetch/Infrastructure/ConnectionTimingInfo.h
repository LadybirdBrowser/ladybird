/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#connection-timing-info
class ConnectionTimingInfo : public JS::Cell {
    JS_CELL(ConnectionTimingInfo, JS::Cell);
    JS_DECLARE_ALLOCATOR(ConnectionTimingInfo);

public:
    [[nodiscard]] static JS::NonnullGCPtr<ConnectionTimingInfo> create(JS::VM&);

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp domain_lookup_start_time() const { return m_domain_lookup_start_time; }
    void set_domain_lookup_start_time(HighResolutionTime::DOMHighResTimeStamp domain_lookup_start_time) { m_domain_lookup_start_time = domain_lookup_start_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp domain_lookup_end_time() const { return m_domain_lookup_end_time; }
    void set_domain_lookup_end_time(HighResolutionTime::DOMHighResTimeStamp domain_lookup_end_time) { m_domain_lookup_end_time = domain_lookup_end_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp connection_start_time() const { return m_connection_start_time; }
    void set_connection_start_time(HighResolutionTime::DOMHighResTimeStamp connection_start_time) { m_connection_start_time = connection_start_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp connection_end_time() const { return m_connection_end_time; }
    void set_connection_end_time(HighResolutionTime::DOMHighResTimeStamp connection_end_time) { m_connection_end_time = connection_end_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp secure_connection_start_time() const { return m_secure_connection_start_time; }
    void set_secure_connection_start_time(HighResolutionTime::DOMHighResTimeStamp secure_connection_start_time) { m_secure_connection_start_time = secure_connection_start_time; }

    [[nodiscard]] ReadonlyBytes lpn_negotiated_protocol() const { return m_lpn_negotiated_protocol; }
    void set_lpn_negotiated_protocol(ByteBuffer lpn_negotiated_protocol) { m_lpn_negotiated_protocol = move(lpn_negotiated_protocol); }

private:
    ConnectionTimingInfo();

    // https://fetch.spec.whatwg.org/#connection-timing-info-domain-lookup-start-time
    // domain lookup start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_domain_lookup_start_time { 0 };

    // https://fetch.spec.whatwg.org/#connection-timing-info-domain-lookup-end-time
    // domain lookup end time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_domain_lookup_end_time { 0 };

    // https://fetch.spec.whatwg.org/#connection-timing-info-connection-start-time
    // connection start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_connection_start_time { 0 };

    // https://fetch.spec.whatwg.org/#connection-timing-info-connection-end-time
    // connection end time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_connection_end_time { 0 };

    // https://fetch.spec.whatwg.org/#connection-timing-info-secure-connection-start-time
    // secure connection start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_secure_connection_start_time { 0 };

    // https://fetch.spec.whatwg.org/#connection-timing-info-alpn-negotiated-protocol
    // ALPN negotiated protocol (default the empty byte sequence)
    //     A byte sequence.
    ByteBuffer m_lpn_negotiated_protocol;
};

}
