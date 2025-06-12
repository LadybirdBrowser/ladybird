/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/MimeTypeBlocking.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#ref-for-should-response-to-request-be-blocked-due-to-mime-type?
RequestOrResponseBlocking should_response_to_request_be_blocked_due_to_its_mime_type(Response const& response, Request const& request)
{
    // 1. Let mimeType be the result of extracting a MIME type from response’s header list.
    auto mime_type = response.header_list()->extract_mime_type();

    // 2. If mimeType is failure, then return allowed.
    if (!mime_type.has_value())
        return RequestOrResponseBlocking::Allowed;

    // 3. Let destination be request’s destination.
    // 4. If destination is script-like and one of the following is true, then return blocked:
    if (request.destination_is_script_like() && (
            // - mimeType’s essence starts with "audio/", "image/", or "video/".
            any_of(Array { "audio/"sv, "image/"sv, "video/"sv }, [&](auto prefix) { return mime_type->essence().starts_with_bytes(prefix); })
            // - mimeType’s essence is "text/csv".
            || mime_type->essence() == "text/csv"sv)) {
        return RequestOrResponseBlocking::Blocked;
    }

    // 5. Return allowed.
    return RequestOrResponseBlocking::Allowed;
}

}
