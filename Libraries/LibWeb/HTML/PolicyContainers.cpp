/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/PolicyContainers.h>

namespace IPC {

template<>
ErrorOr<void> encode(IPC::Encoder& encoder, Web::HTML::PolicyContainer const& policy_container)
{
    TRY(encode(encoder, policy_container.referrer_policy));

    return {};
}

template<>
ErrorOr<Web::HTML::PolicyContainer> decode(IPC::Decoder& decoder)
{
    auto referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>());

    return Web::HTML::PolicyContainer { .referrer_policy = referrer_policy };
}

}
