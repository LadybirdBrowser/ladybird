/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRequests/ALPNHttpVersion.h>

namespace Requests {

struct RequestTimingInfo {
    long domain_lookup_start_microseconds { 0 };
    long domain_lookup_end_microseconds { 0 };
    long connect_start_microseconds { 0 };
    long connect_end_microseconds { 0 };
    long secure_connect_start_microseconds { 0 };
    long request_start_microseconds { 0 };
    long response_start_microseconds { 0 };
    long response_end_microseconds { 0 };
    long encoded_body_size { 0 };
    ALPNHttpVersion http_version_alpn_identifier { ALPNHttpVersion::None };
};

}

namespace IPC {

template<>
inline ErrorOr<void> encode(Encoder& encoder, Requests::RequestTimingInfo const& timing_info)
{
    TRY(encoder.encode(timing_info.domain_lookup_start_microseconds));
    TRY(encoder.encode(timing_info.domain_lookup_end_microseconds));
    TRY(encoder.encode(timing_info.connect_start_microseconds));
    TRY(encoder.encode(timing_info.connect_end_microseconds));
    TRY(encoder.encode(timing_info.secure_connect_start_microseconds));
    TRY(encoder.encode(timing_info.request_start_microseconds));
    TRY(encoder.encode(timing_info.response_start_microseconds));
    TRY(encoder.encode(timing_info.response_end_microseconds));
    TRY(encoder.encode(timing_info.encoded_body_size));
    TRY(encoder.encode(timing_info.http_version_alpn_identifier));
    return {};
}

template<>
inline ErrorOr<Requests::RequestTimingInfo> decode(Decoder& decoder)
{
    auto domain_lookup_start_microseconds = TRY(decoder.decode<long>());
    auto domain_lookup_end_microseconds = TRY(decoder.decode<long>());
    auto connect_start_microseconds = TRY(decoder.decode<long>());
    auto connect_end_microseconds = TRY(decoder.decode<long>());
    auto secure_connect_start_microseconds = TRY(decoder.decode<long>());
    auto request_start_microseconds = TRY(decoder.decode<long>());
    auto response_start_microseconds = TRY(decoder.decode<long>());
    auto response_end_microseconds = TRY(decoder.decode<long>());
    auto encoded_body_size = TRY(decoder.decode<long>());
    auto http_version_alpn_identifier = TRY(decoder.decode<Requests::ALPNHttpVersion>());

    return Requests::RequestTimingInfo {
        .domain_lookup_start_microseconds = domain_lookup_start_microseconds,
        .domain_lookup_end_microseconds = domain_lookup_end_microseconds,
        .connect_start_microseconds = connect_start_microseconds,
        .connect_end_microseconds = connect_end_microseconds,
        .secure_connect_start_microseconds = secure_connect_start_microseconds,
        .request_start_microseconds = request_start_microseconds,
        .response_start_microseconds = response_start_microseconds,
        .response_end_microseconds = response_end_microseconds,
        .encoded_body_size = encoded_body_size,
        .http_version_alpn_identifier = http_version_alpn_identifier,
    };
}

}
