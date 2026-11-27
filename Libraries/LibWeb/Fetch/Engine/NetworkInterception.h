/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibHTTP/Header.h>
#include <LibURL/URL.h>

namespace Web::Fetch::Engine {

enum class NetworkPhase : u8 {
    BeforeRequestSent,
    ResponseStarted,
    AuthRequired,
};

struct ContinueFetch { };

struct FailFetch { };

struct ModifyRequest {
    Optional<URL::URL> url;
    Optional<ByteString> method;
    Optional<Vector<HTTP::Header>> headers;
    Optional<ByteString> cookie;
    Optional<ByteBuffer> body;
};

struct ModifyResponse {
    Optional<u16> status;
    Optional<ByteString> reason;
    Optional<Vector<HTTP::Header>> headers;
    // The values of the response's `Set-Cookie` headers, which go after the headers above if there are any.
    Optional<Vector<ByteString>> set_cookie_values;
};

// Hands the fetch the response the agent describes, built on the one the fetch has, if any.
struct ProvideResponse {
    ModifyResponse response;
    Optional<ByteBuffer> body;
};

// Fetches again with the given `Authorization` value.
struct Authenticate {
    ByteString authorization;
    ModifyResponse response;
};

// Hands the fetch the response that asked for authentication.
struct CancelAuthentication { };

using NetworkDecision = Variant<ContinueFetch, FailFetch, ModifyRequest, ModifyResponse, ProvideResponse, Authenticate, CancelAuthentication>;

}
