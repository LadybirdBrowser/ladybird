/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Directives/StyleSourceAttributeDirective.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(StyleSourceAttributeDirective);

StyleSourceAttributeDirective::StyleSourceAttributeDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

// https://w3c.github.io/webappsec-csp/#style-src-attr-inline
Directive::Result StyleSourceAttributeDirective::inline_check(GC::Heap&, GC::Ptr<DOM::Element const> element, InlineType type, GC::Ref<Policy const> policy, String const& source) const
{
    // 1. Let name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.
    auto name = get_the_effective_directive_for_inline_checks(type);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src-attr and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrcAttr, policy) == ShouldExecute::No)
        return Result::Allowed;

    // 3. If the result of executing § 6.7.3.3 Does element match source list for type and source? on element, this
    //    directive’s value, type, and source, is "Does Not Match", return "Blocked".
    if (does_element_match_source_list_for_type_and_source(element, value(), type, source) == MatchResult::DoesNotMatch)
        return Result::Blocked;

    // 4. Return "Allowed".
    return Result::Allowed;
}

}
