/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/EmbedderPolicy.h>
#include <LibWeb/HTML/Scripting/Environments.h>

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

// https://html.spec.whatwg.org/multipage/browsers.html#compatible-with-cross-origin-isolation
bool is_compatible_with_cross_origin_isolation(EmbedderPolicyValue value)
{
    // An embedder policy value value is compatible with cross-origin isolation if it is "credentialless" or "require-corp".
    return value == EmbedderPolicyValue::Credentialless || value == EmbedderPolicyValue::RequireCorp;
}

// Parses an embedder policy value from a structured field item, ignoring anything that isn't one of the two values that affect the policy.
static Optional<EmbedderPolicyValue> isolating_embedder_policy_value_from_item(Optional<HTTP::StructuredFieldValues::Item> const& item)
{
    if (!item.has_value())
        return {};
    auto token = item->token();
    if (!token.has_value())
        return {};
    if (*token == "require-corp"sv)
        return EmbedderPolicyValue::RequireCorp;
    if (*token == "credentialless"sv)
        return EmbedderPolicyValue::Credentialless;
    return {};
}

static Utf16String reporting_endpoint_from_item(HTTP::StructuredFieldValues::Item const& item)
{
    auto report_to = item.parameters.get("report-to"sv);
    if (!report_to.has_value())
        return {};
    if (auto const* endpoint = report_to->get_pointer<String>())
        return Utf16String::from_utf8(*endpoint);
    return {};
}

// https://html.spec.whatwg.org/multipage/browsers.html#obtain-an-embedder-policy
EmbedderPolicy obtain_an_embedder_policy(Fetch::Infrastructure::Response const& response, Environment const& environment)
{
    // 1. Let policy be a new embedder policy.
    EmbedderPolicy policy;

    // 2. If environment is a non-secure context, then return policy.
    if (is_non_secure_context(environment))
        return policy;

    // 3. Let parsedItem be the result of getting a structured field value with `Cross-Origin-Embedder-Policy` and
    //    "item" from response's header list.
    auto parsed_item = response.header_list()->get_structured_field_item("Cross-Origin-Embedder-Policy"sv);

    // 4. If parsedItem is non-null and parsedItem[0] is "require-corp" or "credentialless":
    if (auto value = isolating_embedder_policy_value_from_item(parsed_item); value.has_value()) {
        // 1. Set policy's value to parsedItem[0].
        policy.value = *value;

        // 2. If parsedItem[1]["report-to"] exists, then set policy's reporting endpoint to parsedItem[1]["report-to"].
        policy.reporting_endpoint = reporting_endpoint_from_item(*parsed_item);
    }

    // 5. Set parsedItem to the result of getting a structured field value with
    //    `Cross-Origin-Embedder-Policy-Report-Only` and "item" from response's header list.
    parsed_item = response.header_list()->get_structured_field_item("Cross-Origin-Embedder-Policy-Report-Only"sv);

    // 6. If parsedItem is non-null and parsedItem[0] is "require-corp" or "credentialless":
    if (auto value = isolating_embedder_policy_value_from_item(parsed_item); value.has_value()) {
        // 1. Set policy's report only value to parsedItem[0].
        policy.report_only_value = *value;

        // 2. If parsedItem[1]["report-to"] exists, then set policy's report only reporting endpoint to parsedItem[1]["report-to"].
        policy.report_only_reporting_endpoint = reporting_endpoint_from_item(*parsed_item);
    }

    // 7. Return policy.
    return policy;
}

// https://html.spec.whatwg.org/multipage/browsers.html#check-a-global-object's-embedder-policy
bool check_a_global_objects_embedder_policy(EmbedderPolicy const& policy, EmbedderPolicy const& owner_policy)
{
    // NOTE: The caller has already established that the global is a WorkerGlobalScope and extracted its embedder
    //       policy and its owner's policy container's embedder policy.
    // FIXME: 4. If ownerPolicy's report-only value is compatible with cross-origin isolation and policy's value is
    //           not, then queue a violation report for COEP inheritance for the worker.

    // 5. If ownerPolicy's value is not compatible with cross-origin isolation or policy's value is compatible with
    //    cross-origin isolation, then return true.
    if (!is_compatible_with_cross_origin_isolation(owner_policy.value) || is_compatible_with_cross_origin_isolation(policy.value))
        return true;

    // FIXME: 6. Queue a violation report for COEP inheritance for the worker.

    // 7. Return false.
    return false;
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
