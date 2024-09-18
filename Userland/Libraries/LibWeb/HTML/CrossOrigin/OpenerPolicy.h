/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/origin.html#cross-origin-opener-policy-value
enum class OpenerPolicyValue {
    UnsafeNone,
    SameOriginAllowPopups,
    SameOrigin,
    SameOriginPlusCOEP,
};

// https://html.spec.whatwg.org/multipage/origin.html#cross-origin-opener-policy
struct OpenerPolicy {
    // A value, which is an opener policy value, initially "unsafe-none".
    OpenerPolicyValue value { OpenerPolicyValue::UnsafeNone };

    // A reporting endpoint, which is string or null, initially null.
    Optional<String> reporting_endpoint;

    // A report-only value, which is an opener policy value, initially "unsafe-none".
    OpenerPolicyValue report_only_value { OpenerPolicyValue::UnsafeNone };

    // A report-only reporting endpoint, which is a string or null, initially null.
    Optional<String> report_only_reporting_endpoint;
};

}
