/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/TrustedTypes/InjectionSink.h>

namespace Web::TrustedTypes {

// https://w3c.github.io/trusted-types/dist/spec/#trusted-types-sink-group
#define ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR \
    __ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR(Script, "'script'")

#define __ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR(name, value) extern Utf16FlyString const& name;
ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR
#undef __ENUMERATE_REQUIRE_KEYWORD_TRUSTED_TYPES_FOR

enum class IncludeReportOnlyPolicies {
    Yes,
    No
};

// https://www.w3.org/TR/trusted-types/#require-trusted-types-for-pre-navigation-check
ContentSecurityPolicy::Directives::Directive::Result require_trusted_types_for_pre_navigation_check(Fetch::Infrastructure::Request&);

bool does_sink_require_trusted_types(JS::Object&, Utf16View, IncludeReportOnlyPolicies);

ContentSecurityPolicy::Directives::Directive::Result should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(JS::Object& global, TrustedTypes::InjectionSink sink, Utf16View sink_group, Utf16String source);

}
