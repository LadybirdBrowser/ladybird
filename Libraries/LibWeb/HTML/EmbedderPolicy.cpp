/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/EmbedderPolicy.h>

namespace Web::HTML {

Utf16View embedder_policy_value_to_string(EmbedderPolicyValue embedder_policy_value)
{
    switch (embedder_policy_value) {
    case EmbedderPolicyValue::UnsafeNone:
        return u"unsafe-none"sv;
    case EmbedderPolicyValue::RequireCorp:
        return u"require-corp"sv;
    case EmbedderPolicyValue::Credentialless:
        return u"credentialless"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<EmbedderPolicyValue> embedder_policy_value_from_string(Utf16View string)
{
    if (string.equals_ignoring_ascii_case(u"unsafe-none"sv))
        return EmbedderPolicyValue::UnsafeNone;
    if (string.equals_ignoring_ascii_case(u"require-corp"sv))
        return EmbedderPolicyValue::RequireCorp;
    if (string.equals_ignoring_ascii_case(u"credentialless"sv))
        return EmbedderPolicyValue::Credentialless;
    return {};
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::EmbedderPolicy const& embedder_policy)
{
    TRY(encoder.encode(embedder_policy.value));
    TRY(encoder.encode(embedder_policy.reporting_endpoint));
    TRY(encoder.encode(embedder_policy.report_only_value));
    TRY(encoder.encode(embedder_policy.report_only_reporting_endpoint));

    return {};
}

template<>
ErrorOr<Web::HTML::EmbedderPolicy> decode(Decoder& decoder)
{
    Web::HTML::EmbedderPolicy embedder_policy {};

    embedder_policy.value = TRY(decoder.decode<Web::HTML::EmbedderPolicyValue>());
    embedder_policy.reporting_endpoint = TRY(decoder.decode<Utf16String>());
    embedder_policy.report_only_value = TRY(decoder.decode<Web::HTML::EmbedderPolicyValue>());
    embedder_policy.report_only_reporting_endpoint = TRY(decoder.decode<Utf16String>());

    return embedder_policy;
}

}
