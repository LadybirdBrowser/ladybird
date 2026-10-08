/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRequests/ExportedRequest.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Requests::ExportedRequest const& request)
{
    TRY(encoder.encode(request.network_isolation_key));
    TRY(encoder.encode(request.url));
    TRY(encoder.encode(request.method));
    TRY(encoder.encode(request.request_headers));
    TRY(encoder.encode(request.request_start_time));
    TRY(encoder.encode(request.status_code));
    TRY(encoder.encode(request.reason_phrase));
    TRY(encoder.encode(request.response_headers));
    TRY(encoder.encode(request.body));
    TRY(encoder.encode(request.status));
    return {};
}

template<>
ErrorOr<Requests::ExportedRequest> decode(Decoder& decoder)
{
    Requests::ExportedRequest request;
    request.network_isolation_key = TRY(decoder.decode<Optional<HTTP::NetworkIsolationKey>>());
    request.url = TRY(decoder.decode<URL::URL>());
    request.method = TRY(decoder.decode<ByteString>());
    request.request_headers = TRY(decoder.decode<Vector<HTTP::Header>>());
    request.request_start_time = TRY(decoder.decode<UnixDateTime>());
    request.status_code = TRY(decoder.decode<Optional<u32>>());
    request.reason_phrase = TRY(decoder.decode<Optional<String>>());
    request.response_headers = TRY(decoder.decode<Vector<HTTP::Header>>());
    request.body = TRY(decoder.decode<IPC::File>());
    request.status = TRY(decoder.decode<IPC::File>());
    return request;
}

}
