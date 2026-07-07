/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::ReferrerPolicy {

static bool equals_ignoring_ascii_case(Utf16View string, StringView ascii_string)
{
    if (string.length_in_code_units() != ascii_string.length())
        return false;

    for (size_t i = 0; i < string.length_in_code_units(); ++i) {
        if (AK::to_ascii_lowercase(string.code_unit_at(i)) != AK::to_ascii_lowercase(ascii_string[i]))
            return false;
    }

    return true;
}

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

Optional<ReferrerPolicy> from_string(Utf16String const& string)
{
    return from_string(string.utf16_view());
}

Optional<ReferrerPolicy> from_string(Utf16View string)
{
    if (string.is_empty())
        return ReferrerPolicy::EmptyString;
    if (equals_ignoring_ascii_case(string, "no-referrer"sv))
        return ReferrerPolicy::NoReferrer;
    if (equals_ignoring_ascii_case(string, "no-referrer-when-downgrade"sv))
        return ReferrerPolicy::NoReferrerWhenDowngrade;
    if (equals_ignoring_ascii_case(string, "same-origin"sv))
        return ReferrerPolicy::SameOrigin;
    if (equals_ignoring_ascii_case(string, "origin"sv))
        return ReferrerPolicy::Origin;
    if (equals_ignoring_ascii_case(string, "strict-origin"sv))
        return ReferrerPolicy::StrictOrigin;
    if (equals_ignoring_ascii_case(string, "origin-when-cross-origin"sv))
        return ReferrerPolicy::OriginWhenCrossOrigin;
    if (equals_ignoring_ascii_case(string, "strict-origin-when-cross-origin"sv))
        return ReferrerPolicy::StrictOriginWhenCrossOrigin;
    if (equals_ignoring_ascii_case(string, "unsafe-url"sv))
        return ReferrerPolicy::UnsafeURL;
    return {};
}

}
