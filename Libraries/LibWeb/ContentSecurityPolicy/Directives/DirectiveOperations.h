/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibGC/Ptr.h>
#include <LibURL/Forward.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

enum class ShouldExecute {
    No,
    Yes,
};

enum class MatchResult {
    DoesNotMatch,
    Matches,
};

[[nodiscard]] Optional<FlyString> get_the_effective_directive_for_request(GC::Ref<Fetch::Infrastructure::Request const> request);
[[nodiscard]] Vector<StringView> get_fetch_directive_fallback_list(Optional<FlyString> directive_name);
[[nodiscard]] ShouldExecute should_fetch_directive_execute(Optional<FlyString> effective_directive_name, FlyString const& directive_name, GC::Ref<Policy const> policy);

[[nodiscard]] FlyString get_the_effective_directive_for_inline_checks(Directive::InlineType type);

[[nodiscard]] MatchResult does_url_match_expression_in_origin_with_redirect_count(URL::URL const& url, String const& expression, URL::Origin const& origin, u8 redirect_count);
[[nodiscard]] MatchResult does_url_match_source_list_in_origin_with_redirect_count(URL::URL const& url, Vector<String> const& source_list, URL::Origin const& origin, u8 redirect_count);

}
