/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/NoSniffBlocking.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#determine-nosniff
bool determine_nosniff(HeaderList const& list)
{
    // 1. Let values be the result of getting, decoding, and splitting `X-Content-Type-Options` from list.
    auto values = list.get_decode_and_split("X-Content-Type-Options"sv.bytes());

    // 2. If values is null, then return false.
    if (!values.has_value())
        return false;

    // 3. If values[0] is an ASCII case-insensitive match for "nosniff", then return true.
    if (!values->is_empty() && Infra::is_ascii_case_insensitive_match(values->at(0), "nosniff"sv))
        return true;

    // 4. Return false.
    return false;
}

// https://fetch.spec.whatwg.org/#should-response-to-request-be-blocked-due-to-nosniff?
RequestOrResponseBlocking should_response_to_request_be_blocked_due_to_nosniff(Response const& response, Request const& request)
{
    // 1. If determine nosniff with response’s header list is false, then return allowed.
    if (!determine_nosniff(response.header_list()))
        return RequestOrResponseBlocking::Allowed;

    // 2. Let mimeType be the result of extracting a MIME type from response’s header list.
    auto mime_type = response.header_list()->extract_mime_type();

    // 3. Let destination be request’s destination.
    auto const& destination = request.destination();

    // 4. If destination is script-like and mimeType is failure or is not a JavaScript MIME type, then return blocked.
    if (request.destination_is_script_like() && (!mime_type.has_value() || !mime_type->is_javascript()))
        return RequestOrResponseBlocking::Blocked;

    // 5. If destination is "style" and mimeType is failure or its essence is not "text/css", then return blocked.
    if (destination == Request::Destination::Style && (!mime_type.has_value() || mime_type->essence() != "text/css"sv))
        return RequestOrResponseBlocking::Blocked;

    // 6. Return allowed.
    return RequestOrResponseBlocking::Allowed;
}

}
