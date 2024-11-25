/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/ContentSecurityPolicy/SerializedPolicy.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::ContentSecurityPolicy::SerializedPolicy const& serialized_policy)
{
    TRY(encoder.encode(serialized_policy.directives));
    TRY(encoder.encode(serialized_policy.disposition));
    TRY(encoder.encode(serialized_policy.source));
    TRY(encoder.encode(serialized_policy.self_origin));

    return {};
}

template<>
ErrorOr<Web::ContentSecurityPolicy::SerializedPolicy> decode(Decoder& decoder)
{
    Web::ContentSecurityPolicy::SerializedPolicy serialized_policy {};

    serialized_policy.directives = TRY(decoder.decode<Vector<Web::ContentSecurityPolicy::Directives::SerializedDirective>>());
    serialized_policy.disposition = TRY(decoder.decode<Web::ContentSecurityPolicy::Policy::Disposition>());
    serialized_policy.source = TRY(decoder.decode<Web::ContentSecurityPolicy::Policy::Source>());
    serialized_policy.self_origin = TRY(decoder.decode<URL::Origin>());

    return serialized_policy;
}

}
