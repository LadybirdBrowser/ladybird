/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/SerializedPolicyContainer.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::SerializedPolicyContainer const& serialized_policy_container)
{
    TRY(encoder.encode(serialized_policy_container.csp_list));
    TRY(encoder.encode(serialized_policy_container.embedder_policy));
    TRY(encoder.encode(serialized_policy_container.referrer_policy));

    return {};
}

template<>
ErrorOr<Web::HTML::SerializedPolicyContainer> decode(Decoder& decoder)
{
    Web::HTML::SerializedPolicyContainer serialized_policy_container {};

    serialized_policy_container.csp_list = TRY(decoder.decode<Vector<Web::ContentSecurityPolicy::SerializedPolicy>>());
    serialized_policy_container.embedder_policy = TRY(decoder.decode<Web::HTML::EmbedderPolicy>());
    serialized_policy_container.referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>());

    return serialized_policy_container;
}

}
