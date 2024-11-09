/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-value
enum class EmbedderPolicyValue {
    UnsafeNone,
    RequireCorp,
    Credentialless,
};

StringView embedder_policy_value_to_string(EmbedderPolicyValue);
Optional<EmbedderPolicyValue> embedder_policy_value_from_string(StringView);

// https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy
struct EmbedderPolicy {
    // https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-value-2
    // A value, which is an embedder policy value, initially "unsafe-none".
    EmbedderPolicyValue value { EmbedderPolicyValue::UnsafeNone };

    // https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-reporting-endpoint
    // A reporting endpoint string, initially the empty string.
    String reporting_endpoint;

    // https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-report-only-value
    // A report only value, which is an embedder policy value, initially "unsafe-none".
    EmbedderPolicyValue report_only_value { EmbedderPolicyValue::UnsafeNone };

    // https://html.spec.whatwg.org/multipage/browsers.html#embedder-policy-report-only-reporting-endpoint
    // A report only reporting endpoint string, initially the empty string.
    String report_only_reporting_endpoint;
};

}
