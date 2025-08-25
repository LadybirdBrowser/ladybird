/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/FormActionDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(FormActionDirective);

FormActionDirective::FormActionDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

Directive::Result FormActionDirective::pre_navigation_check(GC::Ref<Fetch::Infrastructure::Request> request, NavigationType navigation_type, GC::Ref<Policy const> policy) const
{
    // 1. Assert: policy is unused in this algorithm.
    // FIXME: File spec issue, because this is not the case. The policy is required to resolve 'self'.

    // 2. If navigation type is "form-submission":
    if (navigation_type == NavigationType::FormSubmission) {
        // 1. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
        //    and a policy, is "Does Not Match", return "Blocked".
        if (does_request_match_source_list(request, value(), policy) == MatchResult::DoesNotMatch)
            return Result::Blocked;
    }

    // 3. Return "Allowed".
    return Result::Allowed;
}

}
