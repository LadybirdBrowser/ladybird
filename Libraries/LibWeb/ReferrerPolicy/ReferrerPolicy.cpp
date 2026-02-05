/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::ReferrerPolicy {

StringView to_string(ReferrerPolicy referrer_policy)
{
    switch (referrer_policy) {
    case ReferrerPolicy::EmptyString:
        return ""sv;
    case ReferrerPolicy::NoReferrer:
        return "no-referrer"sv;
    case ReferrerPolicy::NoReferrerWhenDowngrade:
        return "no-referrer-when-downgrade"sv;
    case ReferrerPolicy::SameOrigin:
        return "same-origin"sv;
    case ReferrerPolicy::Origin:
        return "origin"sv;
    case ReferrerPolicy::StrictOrigin:
        return "strict-origin"sv;
    case ReferrerPolicy::OriginWhenCrossOrigin:
        return "origin-when-cross-origin"sv;
    case ReferrerPolicy::StrictOriginWhenCrossOrigin:
        return "strict-origin-when-cross-origin"sv;
    case ReferrerPolicy::UnsafeURL:
        return "unsafe-url"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<ReferrerPolicy> from_string(StringView string)
{
    if (string.is_empty())
        return ReferrerPolicy::EmptyString;
    if (string.equals_ignoring_ascii_case("no-referrer"sv))
        return ReferrerPolicy::NoReferrer;
    if (string.equals_ignoring_ascii_case("no-referrer-when-downgrade"sv))
        return ReferrerPolicy::NoReferrerWhenDowngrade;
    if (string.equals_ignoring_ascii_case("same-origin"sv))
        return ReferrerPolicy::SameOrigin;
    if (string.equals_ignoring_ascii_case("origin"sv))
        return ReferrerPolicy::Origin;
    if (string.equals_ignoring_ascii_case("strict-origin"sv))
        return ReferrerPolicy::StrictOrigin;
    if (string.equals_ignoring_ascii_case("origin-when-cross-origin"sv))
        return ReferrerPolicy::OriginWhenCrossOrigin;
    if (string.equals_ignoring_ascii_case("strict-origin-when-cross-origin"sv))
        return ReferrerPolicy::StrictOriginWhenCrossOrigin;
    if (string.equals_ignoring_ascii_case("unsafe-url"sv))
        return ReferrerPolicy::UnsafeURL;
    return {};
}

}
