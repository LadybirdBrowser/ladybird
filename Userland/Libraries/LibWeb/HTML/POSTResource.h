/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#post-resource
struct POSTResource {
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#post-resource-request-body
    // A request body, a byte sequence or failure.
    // FIXME: Change type to hold failure state.
    Optional<ByteBuffer> request_body;

    enum class RequestContentType {
        ApplicationXWWWFormUrlencoded,
        MultipartFormData,
        TextPlain,
    };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#post-resource-request-content-type
    // A request content-type, which is `application/x-www-form-urlencoded`, `multipart/form-data`, or `text/plain`.
    RequestContentType request_content_type {};

    struct Directive {
        StringView type;
        String value;
    };
    Vector<Directive> request_content_type_directives {};
};

}
