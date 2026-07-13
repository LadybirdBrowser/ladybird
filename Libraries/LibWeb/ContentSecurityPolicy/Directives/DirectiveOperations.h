/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibURL/Forward.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

enum class [[nodiscard]] ShouldExecute {
    No,
    Yes,
};

enum class [[nodiscard]] MatchResult {
    DoesNotMatch,
    Matches,
};

[[nodiscard]] Optional<Utf16FlyString> get_the_effective_directive_for_request(GC::Ref<Fetch::Infrastructure::Request const> request);
[[nodiscard]] Vector<Utf16FlyString> get_fetch_directive_fallback_list(Optional<Utf16FlyString> directive_name);
ShouldExecute should_fetch_directive_execute(Optional<Utf16FlyString> effective_directive_name, Utf16FlyString const& directive_name, GC::Ref<Policy const> policy);

[[nodiscard]] Utf16FlyString get_the_effective_directive_for_inline_checks(Directive::InlineType type);

MatchResult does_url_match_expression_in_origin_with_redirect_count(URL::URL const& url, Utf16View expression, URL::Origin const& origin, u8 redirect_count);
MatchResult does_url_match_source_list_in_origin_with_redirect_count(URL::URL const& url, Vector<Utf16String> const& source_list, URL::Origin const& origin, u8 redirect_count);

MatchResult does_request_match_source_list(GC::Ref<Fetch::Infrastructure::Request const> request, Vector<Utf16String> const& source_list, GC::Ref<Policy const> policy);
MatchResult does_response_match_source_list(GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ref<Fetch::Infrastructure::Request const> request, Vector<Utf16String> const& source_list, GC::Ref<Policy const> policy);
MatchResult does_nonce_match_source_list(Utf16View nonce, Vector<Utf16String> const& source_list);

Directive::Result script_directives_pre_request_check(GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Directive const> directive, GC::Ref<Policy const> policy);
Directive::Result script_directives_post_request_check(GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ref<Directive const> directive, GC::Ref<Policy const> policy);

MatchResult does_element_match_source_list_for_type_and_source(GC::Ptr<DOM::Element const> element, Vector<Utf16String> const& source_list, Directive::InlineType type, Utf16View source);

}
