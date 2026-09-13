/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-value
enum class EmbedderPolicyValue : u8 {
    UnsafeNone,
    RequireCorp,
    Credentialless,
};

Utf16View embedder_policy_value_to_string(EmbedderPolicyValue);
Optional<EmbedderPolicyValue> embedder_policy_value_from_string(Utf16View);

// https://html.spec.whatwg.org/multipage/browsers.html#compatible-with-cross-origin-isolation
WEB_API bool is_compatible_with_cross_origin_isolation(EmbedderPolicyValue);

// https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy
struct EmbedderPolicy {
    // https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-value-2
    // A value, which is an embedder policy value, initially "unsafe-none".
    EmbedderPolicyValue value { EmbedderPolicyValue::UnsafeNone };

    // https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-report-only-value
    // A report only value, which is an embedder policy value, initially "unsafe-none".
    EmbedderPolicyValue report_only_value { EmbedderPolicyValue::UnsafeNone };

    // https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-reporting-endpoint
    // A reporting endpoint string, initially the empty string.
    Utf16String reporting_endpoint;

    // https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-report-only-reporting-endpoint
    // A report only reporting endpoint string, initially the empty string.
    Utf16String report_only_reporting_endpoint;
};

// https://html.spec.whatwg.org/multipage/browsers.html#obtain-an-embedder-policy
WEB_API EmbedderPolicy obtain_an_embedder_policy(Fetch::Infrastructure::Response const&, Environment const&);

// https://html.spec.whatwg.org/multipage/browsers.html#check-a-global-object's-embedder-policy
WEB_API bool check_a_global_objects_embedder_policy(EmbedderPolicy const& policy, EmbedderPolicy const& owner_policy);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::EmbedderPolicy const&);

template<>
ErrorOr<Web::HTML::EmbedderPolicy> decode(Decoder&);

}
