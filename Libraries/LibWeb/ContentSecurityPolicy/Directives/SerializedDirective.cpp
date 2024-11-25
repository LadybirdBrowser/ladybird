/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/ContentSecurityPolicy/Directives/SerializedDirective.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::ContentSecurityPolicy::Directives::SerializedDirective const& serialized_directive)
{
    TRY(encoder.encode(serialized_directive.name));
    TRY(encoder.encode(serialized_directive.value));

    return {};
}

template<>
ErrorOr<Web::ContentSecurityPolicy::Directives::SerializedDirective> decode(Decoder& decoder)
{
    Web::ContentSecurityPolicy::Directives::SerializedDirective serialized_directive {};

    serialized_directive.name = TRY(decoder.decode<String>());
    serialized_directive.value = TRY(decoder.decode<Vector<String>>());

    return serialized_directive;
}

}
