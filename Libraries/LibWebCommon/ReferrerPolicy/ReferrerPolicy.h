/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWebCommon/Export.h>

namespace Web::ReferrerPolicy {

// https://w3c.github.io/webappsec-referrer-policy/#enumdef-referrerpolicy
enum class ReferrerPolicy {
    EmptyString,
    NoReferrer,
    NoReferrerWhenDowngrade,
    SameOrigin,
    Origin,
    StrictOrigin,
    OriginWhenCrossOrigin,
    StrictOriginWhenCrossOrigin,
    UnsafeURL,
};

// https://w3c.github.io/webappsec-referrer-policy/#default-referrer-policy
// The default referrer policy is "strict-origin-when-cross-origin".
constexpr auto DEFAULT_REFERRER_POLICY = ReferrerPolicy::StrictOriginWhenCrossOrigin;

WEBCOMMON_API StringView to_string(ReferrerPolicy);
WEBCOMMON_API Optional<ReferrerPolicy> from_string(StringView);
WEBCOMMON_API Optional<ReferrerPolicy> from_string(Utf16View);
WEBCOMMON_API Optional<ReferrerPolicy> from_string(Utf16String const&);

}
