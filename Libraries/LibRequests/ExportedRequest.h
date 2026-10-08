/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibHTTP/Header.h>
#include <LibHTTP/NetworkIsolationKey.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>

namespace Requests {

// A response moving between RequestServers. The body socket streams the body; the status socket receives the final
// transfer outcome. Closing the status socket tells the exporter that nobody is waiting for the response.
struct ExportedRequest {
    Optional<HTTP::NetworkIsolationKey> network_isolation_key;
    URL::URL url;
    ByteString method;
    Vector<HTTP::Header> request_headers;
    UnixDateTime request_start_time;

    Optional<u32> status_code;
    Optional<String> reason_phrase;
    Vector<HTTP::Header> response_headers;

    IPC::File body;
    IPC::File status;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::ExportedRequest const&);

template<>
ErrorOr<Requests::ExportedRequest> decode(Decoder&);

}
