/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/FrameAncestorsDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/Navigable.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(FrameAncestorsDirective);

FrameAncestorsDirective::FrameAncestorsDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

// https://w3c.github.io/webappsec-csp/#frame-ancestors-navigation-response
Directive::Result FrameAncestorsDirective::navigation_response_check(GC::Ref<Fetch::Infrastructure::Request const>, NavigationType, GC::Ref<Fetch::Infrastructure::Response const> navigation_response, GC::Ref<HTML::Navigable const> target, CheckType check_type, GC::Ref<Policy const> policy) const
{
    // 1. If navigation response’s URL is local, return "Allowed".
    VERIFY(navigation_response->url().has_value());
    if (Fetch::Infrastructure::is_local_url(navigation_response->url().value()))
        return Result::Allowed;

    // 2. Assert: request, navigation response, and navigation type, are unused from this point forward in this
    //    algorithm, as frame-ancestors is concerned only with navigation response’s frame-ancestors directive.

    // 3. If check type is "source", return "Allowed".
    // Spec Note: The 'frame-ancestors' directive is relevant only to the target navigable and it has no impact on the
    //            request’s context.
    if (check_type == CheckType::Source)
        return Result::Allowed;

    // 4. If target is not a child navigable, return "Allowed".
    if (!target->parent())
        return Result::Allowed;

    // 5. Let current be target.
    auto current = target;

    // 6. While current is a child navigable:
    while (current->parent()) {
        // 1. Let document be current’s container document.
        auto document = current->container_document();
        VERIFY(document);

        // 2. Let origin be the result of executing the URL parser on the ASCII serialization of document’s origin.
        auto origin = DOMURL::parse(document->origin().serialize());

        // FIXME: What do we do if origin is invalid here?
        VERIFY(origin.has_value());

        // 3. If § 6.7.2.7 Does url match source list in origin with redirect count? returns Does Not Match when
        //    executed upon origin, this directive’s value, policy’s self-origin, and 0, return "Blocked".
        if (does_url_match_source_list_in_origin_with_redirect_count(origin.value(), value(), policy->self_origin(), 0) == MatchResult::DoesNotMatch)
            return Result::Blocked;

        // 4. Set current to document’s node navigable.
        VERIFY(document->navigable());
        current = *document->navigable();
    }

    // 7. Return "Allowed".
    return Result::Allowed;
}

}
