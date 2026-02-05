/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Forward.h>
#include <LibRequests/ALPNHttpVersion.h>

namespace Requests {

struct RequestTimingInfo {
    i64 domain_lookup_start_microseconds { 0 };
    i64 domain_lookup_end_microseconds { 0 };
    i64 connect_start_microseconds { 0 };
    i64 connect_end_microseconds { 0 };
    i64 secure_connect_start_microseconds { 0 };
    i64 request_start_microseconds { 0 };
    i64 response_start_microseconds { 0 };
    i64 response_end_microseconds { 0 };
    i64 encoded_body_size { 0 };
    ALPNHttpVersion http_version_alpn_identifier { ALPNHttpVersion::None };
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::RequestTimingInfo const&);

template<>
ErrorOr<Requests::RequestTimingInfo> decode(Decoder&);

}
