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
        return ""_sv;
    case ReferrerPolicy::NoReferrer:
        return "no-referrer"_sv;
    case ReferrerPolicy::NoReferrerWhenDowngrade:
        return "no-referrer-when-downgrade"_sv;
    case ReferrerPolicy::SameOrigin:
        return "same-origin"_sv;
    case ReferrerPolicy::Origin:
        return "origin"_sv;
    case ReferrerPolicy::StrictOrigin:
        return "strict-origin"_sv;
    case ReferrerPolicy::OriginWhenCrossOrigin:
        return "origin-when-cross-origin"_sv;
    case ReferrerPolicy::StrictOriginWhenCrossOrigin:
        return "strict-origin-when-cross-origin"_sv;
    case ReferrerPolicy::UnsafeURL:
        return "unsafe-url"_sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<ReferrerPolicy> from_string(StringView string)
{
    if (string.is_empty())
        return ReferrerPolicy::EmptyString;
    if (string.equals_ignoring_ascii_case("no-referrer"_sv))
        return ReferrerPolicy::NoReferrer;
    if (string.equals_ignoring_ascii_case("no-referrer-when-downgrade"_sv))
        return ReferrerPolicy::NoReferrerWhenDowngrade;
    if (string.equals_ignoring_ascii_case("same-origin"_sv))
        return ReferrerPolicy::SameOrigin;
    if (string.equals_ignoring_ascii_case("origin"_sv))
        return ReferrerPolicy::Origin;
    if (string.equals_ignoring_ascii_case("strict-origin"_sv))
        return ReferrerPolicy::StrictOrigin;
    if (string.equals_ignoring_ascii_case("origin-when-cross-origin"_sv))
        return ReferrerPolicy::OriginWhenCrossOrigin;
    if (string.equals_ignoring_ascii_case("strict-origin-when-cross-origin"_sv))
        return ReferrerPolicy::StrictOriginWhenCrossOrigin;
    if (string.equals_ignoring_ascii_case("unsafe-url"_sv))
        return ReferrerPolicy::UnsafeURL;
    return {};
}

}
