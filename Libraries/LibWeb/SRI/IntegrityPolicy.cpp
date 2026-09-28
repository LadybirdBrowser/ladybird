/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/SRI/IntegrityPolicy.h>
#include <LibWeb/SRI/SRI.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>

namespace Web::SRI {

// https://w3c.github.io/webappsec-subresource-integrity/#should-request-be-blocked-by-integrity-policy
ContentSecurityPolicy::Directives::Directive::Result should_request_be_blocked_by_integrity_policy(GC::Ref<Fetch::Infrastructure::Request> request)
{
    VERIFY(request->policy_container().has<NonnullRefPtr<HTML::PolicyContainer const>>());

    // 1. Let policyContainer be request’s policy container.
    auto const& policy_container = request->policy_container().get<NonnullRefPtr<HTML::PolicyContainer const>>();

    // 2. Let parsedMetadata be the result of calling parse metadata with request’s integrity metadata.
    auto parsed_metadata = MUST(parse_metadata(request->integrity_metadata().utf16_view()));

    // 3. If parsedMetadata is not the empty set and request’s mode is either "cors" or "same-origin", return "Allowed".
    if (!parsed_metadata.is_empty() && (request->mode() == Fetch::Infrastructure::Request::Mode::CORS || request->mode() == Fetch::Infrastructure::Request::Mode::SameOrigin))
        return ContentSecurityPolicy::Directives::Directive::Result::Allowed;

    // 4. If request’s url is local, return "Allowed".
    if (Fetch::Infrastructure::is_local_url(request->url()))
        return ContentSecurityPolicy::Directives::Directive::Result::Allowed;

    // 5. Let policy be policyContainer’s integrity policy.
    auto const& policy = policy_container->integrity_policy;

    // 6. Let reportPolicy be policyContainer’s report only integrity policy.
    auto const& report_policy = policy_container->report_only_integrity_policy;

    // 7. If both policy and reportPolicy are empty integrity policys, return "Allowed".
    if (policy.is_empty() && report_policy.is_empty())
        return ContentSecurityPolicy::Directives::Directive::Result::Allowed;

    // 8. Let global be request’s client’s global object.
    auto& global = request->client()->global_object();

    // 9. If global is not a Window nor a WorkerGlobalScope, return "Allowed".
    if (!HTML::window_or_worker_global_scope_from_global_object(global))
        return ContentSecurityPolicy::Directives::Directive::Result::Allowed;

    // 10. Let block be a boolean, initially false.
    bool block = false;

    // FIXME: 11. Let reportBlock be a boolean, initially false.
    [[maybe_unused]] auto report_block = false;

    // 12. If policy’s sources contains "inline" and policy’s blocked destinations contains request’s destination, set block to true.
    if (policy.sources.contains_slow(u"inline"sv)
        && request->destination().has_value()
        && policy.blocked_destinations.contains_slow(request->destination().value()))
        block = true;

    // 13. If reportPolicy’s sources contains "inline" and reportPolicy’s blocked destinations contains request’s destination, set reportBlock to true.
    if (report_policy.sources.contains_slow(u"inline"sv)
        && request->destination().has_value()
        && report_policy.blocked_destinations.contains_slow(request->destination().value()))
        report_block = true;

    // FIXME: 14. If block is true or reportBlock is true, then report violation with request, block, reportBlock, policy and reportPolicy.

    // 15. If block is true, then return "Blocked"; otherwise "Allowed".
    return block ? ContentSecurityPolicy::Directives::Directive::Result::Blocked : ContentSecurityPolicy::Directives::Directive::Result::Allowed;
}

}
