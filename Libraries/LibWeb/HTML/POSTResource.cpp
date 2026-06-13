/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/POSTResource.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::POSTResource::Directive const& directive)
{
    TRY(encoder.encode(directive.type));
    TRY(encoder.encode(directive.value));
    return {};
}

template<>
ErrorOr<Web::HTML::POSTResource::Directive> decode(Decoder& decoder)
{
    return Web::HTML::POSTResource::Directive {
        .type = TRY(decoder.decode<String>()),
        .value = TRY(decoder.decode<String>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::POSTResource const& post_resource)
{
    TRY(encoder.encode(post_resource.request_body));
    TRY(encoder.encode(post_resource.request_content_type));
    TRY(encoder.encode(post_resource.request_content_type_directives));
    return {};
}

template<>
ErrorOr<Web::HTML::POSTResource> decode(Decoder& decoder)
{
    return Web::HTML::POSTResource {
        .request_body = TRY(decoder.decode<Optional<ByteBuffer>>()),
        .request_content_type = TRY(decoder.decode<Web::HTML::POSTResource::RequestContentType>()),
        .request_content_type_directives = TRY(decoder.decode<Vector<Web::HTML::POSTResource::Directive>>()),
    };
}

}
