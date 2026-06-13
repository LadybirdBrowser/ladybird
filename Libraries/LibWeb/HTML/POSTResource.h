/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Forward.h>

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
        String type;
        String value;

        bool operator==(Directive const&) const = default;
    };
    Vector<Directive> request_content_type_directives {};

    bool operator==(POSTResource const&) const = default;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::POSTResource::Directive const&);

template<>
WEB_API ErrorOr<Web::HTML::POSTResource::Directive> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::POSTResource const&);

template<>
WEB_API ErrorOr<Web::HTML::POSTResource> decode(Decoder&);

}
