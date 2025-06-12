/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Checks.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::Fetch::Fetching {

// https://fetch.spec.whatwg.org/#concept-cors-check
bool cors_check(Infrastructure::Request const& request, Infrastructure::Response const& response)
{
    // 1. Let origin be the result of getting `Access-Control-Allow-Origin` from response’s header list.
    auto origin = response.header_list()->get("Access-Control-Allow-Origin"sv.bytes());

    // 2. If origin is null, then return failure.
    // NOTE: Null is not `null`.
    if (!origin.has_value())
        return false;

    // 3. If request’s credentials mode is not "include" and origin is `*`, then return success.
    if (request.credentials_mode() != Infrastructure::Request::CredentialsMode::Include && origin->span() == "*"sv.bytes())
        return true;

    // 4. If the result of byte-serializing a request origin with request is not origin, then return failure.
    if (request.byte_serialize_origin() != *origin)
        return false;

    // 5. If request’s credentials mode is not "include", then return success.
    if (request.credentials_mode() != Infrastructure::Request::CredentialsMode::Include)
        return true;

    // 6. Let credentials be the result of getting `Access-Control-Allow-Credentials` from response’s header list.
    auto credentials = response.header_list()->get("Access-Control-Allow-Credentials"sv.bytes());

    // 7. If credentials is `true`, then return success.
    if (credentials.has_value() && credentials->span() == "true"sv.bytes())
        return true;

    // 8. Return failure.
    return false;
}

// https://fetch.spec.whatwg.org/#concept-tao-check
bool tao_check(Infrastructure::Request const& request, Infrastructure::Response const& response)
{
    // 1. If request’s timing allow failed flag is set, then return failure.
    if (request.timing_allow_failed())
        return false;

    // 2. Let values be the result of getting, decoding, and splitting `Timing-Allow-Origin` from response’s header list.
    auto values = response.header_list()->get_decode_and_split("Timing-Allow-Origin"sv.bytes());

    // 3. If values contains "*", then return success.
    if (values.has_value() && values->contains_slow("*"sv))
        return true;

    // 4. If values contains the result of serializing a request origin with request, then return success.
    if (values.has_value() && values->contains_slow(request.serialize_origin()))
        return true;

    // 5. If request’s mode is "navigate" and request’s current URL’s origin is not same origin with request’s origin, then return failure.
    // NOTE: This is necessary for navigations of a nested browsing context. There, request’s origin would be the
    //       container document’s origin and the TAO check would return failure. Since navigation timing never
    //       validates the results of the TAO check, the nested document would still have access to the full timing
    //       information, but the container document would not.
    if (request.mode() == Infrastructure::Request::Mode::Navigate
        && request.origin().has<URL::Origin>()
        && !request.current_url().origin().is_same_origin(request.origin().get<URL::Origin>())) {
        return false;
    }

    // 6. If request’s response tainting is "basic", then return success.
    if (request.response_tainting() == Infrastructure::Request::ResponseTainting::Basic)
        return true;

    // 7. Return failure.
    return false;
}

}
