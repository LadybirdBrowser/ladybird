/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsers.html#compatible-with-cross-origin-isolation
bool is_compatible_with_cross_origin_isolation(EmbedderPolicyValue value)
{
    // An embedder policy value is compatible with cross-origin isolation if it is "credentialless" or "require-corp".
    return value == EmbedderPolicyValue::Credentialless || value == EmbedderPolicyValue::RequireCorp;
}

static Optional<EmbedderPolicyValue> embedder_policy_value_of_token(Optional<HTTP::StructuredFields::Item> const& parsed_item)
{
    if (!parsed_item.has_value())
        return {};
    auto const* token = parsed_item->bare_item.get_pointer<HTTP::StructuredFields::Token>();
    if (!token)
        return {};
    if (token->value == "unsafe-none"sv)
        return EmbedderPolicyValue::UnsafeNone;
    if (token->value == "require-corp"sv)
        return EmbedderPolicyValue::RequireCorp;
    if (token->value == "credentialless"sv)
        return EmbedderPolicyValue::Credentialless;
    return {};
}

// https://html.spec.whatwg.org/multipage/browsers.html#obtain-an-embedder-policy
// NB: response is given as its header list, and environment as whether it is a non-secure context, which is all these
//     steps read of them.
EmbedderPolicy obtain_an_embedder_policy(HTTP::HeaderList const& header_list, bool environment_is_a_non_secure_context)
{
    // 1. Let policy be a new embedder policy.
    EmbedderPolicy policy;

    // 2. If environment is a non-secure context, then return policy.
    if (environment_is_a_non_secure_context)
        return policy;

    // 3. Let parsedItem be the result of getting a structured field value with `Cross-Origin-Embedder-Policy` and
    //    "item" from response's header list.
    auto parsed_item = header_list.get_structured_field_value<HTTP::StructuredFields::Item>("Cross-Origin-Embedder-Policy"sv);

    // 4. If parsedItem is non-null, parsedItem[0] is a token, and parsedItem[0] is compatible with cross-origin
    //    isolation:
    if (auto value = embedder_policy_value_of_token(parsed_item); value.has_value() && is_compatible_with_cross_origin_isolation(*value)) {
        // 1. Set policy's value to parsedItem[0].
        policy.value = *value;

        // 2. If parsedItem[1]["report-to"] exists and it is a string, then set policy's reporting endpoint to
        //    parsedItem[1]["report-to"].
        if (auto report_to = parsed_item->parameters.get("report-to"sv); report_to.has_value() && report_to->has<ByteString>())
            policy.reporting_endpoint = Utf16String::from_utf8(report_to->get<ByteString>());
    }

    // 5. Set parsedItem to the result of getting a structured field value with
    //    `Cross-Origin-Embedder-Policy-Report-Only` and "item" from response's header list.
    parsed_item = header_list.get_structured_field_value<HTTP::StructuredFields::Item>("Cross-Origin-Embedder-Policy-Report-Only"sv);

    // 6. If parsedItem is non-null, parsedItem[0] is a token, and parsedItem[0] is compatible with cross-origin
    //    isolation:
    if (auto value = embedder_policy_value_of_token(parsed_item); value.has_value() && is_compatible_with_cross_origin_isolation(*value)) {
        // 1. Set policy's report-only value to parsedItem[0].
        policy.report_only_value = *value;

        // 2. If parsedItem[1]["report-to"] exists and it is a string, then set policy's report-only reporting endpoint
        //    to parsedItem[1]["report-to"].
        if (auto report_to = parsed_item->parameters.get("report-to"sv); report_to.has_value() && report_to->has<ByteString>())
            policy.report_only_reporting_endpoint = Utf16String::from_utf8(report_to->get<ByteString>());
    }

    // 7. Return policy.
    return policy;
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
