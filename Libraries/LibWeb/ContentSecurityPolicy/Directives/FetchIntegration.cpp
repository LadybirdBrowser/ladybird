/*
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/FetchIntegration.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#child-src-pre-request
static Directive::Result child_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, child-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ChildSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. Return the result of executing the pre-request check for the directive whose name is name on request and
    //    policy, using this directive’s value for the comparison.
    return pre_request_check(Directive::create(*name, directive.value()), request, policy);
}

// https://w3c.github.io/webappsec-csp/#connect-src-pre-request
static Directive::Result connect_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, connect-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ConnectSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
    //    and policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#default-src-pre-request
static Directive::Result default_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, default-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::DefaultSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. Return the result of executing the pre-request check for the directive whose name is name on request and
    //    policy, using this directive’s value for the comparison.
    return pre_request_check(Directive::create(*name, directive.value()), request, policy);
}

// https://w3c.github.io/webappsec-csp/#font-src-pre-request
static Directive::Result font_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, font-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::FontSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
    //    and policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#frame-src-pre-request
static Directive::Result frame_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, frame-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::FrameSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
    //    and policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#img-src-pre-request
static Directive::Result img_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, img-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ImgSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
    //    and policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#manifest-src-pre-request
static Directive::Result manifest_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, manifest-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ManifestSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
    //    and policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#media-src-pre-request
static Directive::Result media_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, media-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::MediaSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
    //    and policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#object-src-pre-request
static Directive::Result object_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, object-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ObjectSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
    //    and policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#script-src-pre-request
static Directive::Result script_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, script-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ScriptSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. Return the result of executing § 6.7.1.1 Script directives pre-request check on request, this directive,
    //    and policy.
    return script_directives_pre_request_check(request, directive, policy);
}

// https://w3c.github.io/webappsec-csp/#script-src-elem-pre-request
static Directive::Result script_src_elem_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, script-src-elem and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ScriptSrcElem, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. Return the result of executing § 6.7.1.1 Script directives pre-request check on request, this directive,
    //    and policy.
    return script_directives_pre_request_check(request, directive, policy);
}

// https://w3c.github.io/webappsec-csp/#style-src-pre-request
static Directive::Result style_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.3 Does nonce match source list? on request’s cryptographic nonce metadata
    //    and this directive’s value is "Matches", return "Allowed".
    if (does_nonce_match_source_list(request.cryptographic_nonce_metadata(), directive.value()) == MatchResult::Matches)
        return Directive::Result::Allowed;

    // 4. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value, and
    //    policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 5. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#style-src-elem-pre-request
static Directive::Result style_src_elem_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src-elem and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrcElem, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.3 Does nonce match source list? on request’s cryptographic nonce metadata
    //    and this directive’s value is "Matches", return "Allowed".
    if (does_nonce_match_source_list(request.cryptographic_nonce_metadata(), directive.value()) == MatchResult::Matches)
        return Directive::Result::Allowed;

    // 4. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value, and
    //    policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 5. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#worker-src-pre-request
static Directive::Result worker_src_pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, worker-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::WorkerSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.5 Does request match source list? on request, this directive’s value,
    //    and policy, is "Does Not Match", return "Blocked".
    if (does_request_match_source_list(request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#child-src-post-request
static Directive::Result child_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, child-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ChildSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. Return the result of executing the post-request check for the directive whose name is name on request,
    //    response, and policy, using this directive’s value for the comparison.
    return post_request_check(Directive::create(*name, directive.value()), request, response, policy);
}

// https://w3c.github.io/webappsec-csp/#connect-src-post-request
static Directive::Result connect_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, connect-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ConnectSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#default-src-post-request
static Directive::Result default_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, default-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::DefaultSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. Return the result of executing the post-request check for the directive whose name is name on request,
    //    response, and policy, using this directive’s value for the comparison.
    return post_request_check(Directive::create(*name, directive.value()), request, response, policy);
}

// https://w3c.github.io/webappsec-csp/#font-src-post-request
static Directive::Result font_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, font-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::FontSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#frame-src-post-request
static Directive::Result frame_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, frame-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::FrameSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#img-src-post-request
static Directive::Result img_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, img-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ImgSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#manifest-src-post-request
static Directive::Result manifest_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, manifest-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ManifestSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#media-src-post-request
static Directive::Result media_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, media-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::MediaSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#object-src-post-request
static Directive::Result object_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, object-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ObjectSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#script-src-post-request
static Directive::Result script_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, script-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ScriptSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. Return the result of executing § 6.7.1.2 Script directives post-request check on request, response, this
    //    directive, and policy.
    return script_directives_post_request_check(request, response, directive, policy);
}

// https://w3c.github.io/webappsec-csp/#script-src-elem-post-request
static Directive::Result script_src_elem_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, script-src-elem and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::ScriptSrcElem, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. Return the result of executing § 6.7.1.2 Script directives post-request check on request, response, this
    //    directive, and policy.
    return script_directives_post_request_check(request, response, directive, policy);
}

// https://w3c.github.io/webappsec-csp/#style-src-post-request
static Directive::Result style_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.3 Does nonce match source list? on request’s cryptographic nonce metadata
    //    and this directive’s value is "Matches", return "Allowed".
    if (does_nonce_match_source_list(request.cryptographic_nonce_metadata(), directive.value()) == MatchResult::Matches)
        return Directive::Result::Allowed;

    // 4. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 5. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#style-src-elem-post-request
static Directive::Result style_src_elem_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, style-src-elem and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::StyleSrcElem, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.3 Does nonce match source list? on request’s cryptographic nonce metadata
    //    and this directive’s value is "Matches", return "Allowed".
    if (does_nonce_match_source_list(request.cryptographic_nonce_metadata(), directive.value()) == MatchResult::Matches)
        return Directive::Result::Allowed;

    // 4. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 5. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#worker-src-post-request
static Directive::Result worker_src_post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto name = get_the_effective_directive_for_request(request);

    // 2. If the result of executing § 6.8.4 Should fetch directive execute on name, worker-src and policy is "No",
    //    return "Allowed".
    if (should_fetch_directive_execute(name, Names::WorkerSrc, policy) == ShouldExecute::No)
        return Directive::Result::Allowed;

    // 3. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request, this
    //    directive’s value, and policy, is "Does Not Match", return "Blocked".
    if (does_response_match_source_list(response, request, directive.value(), policy) == MatchResult::DoesNotMatch)
        return Directive::Result::Blocked;

    // 4. Return "Allowed".
    return Directive::Result::Allowed;
}

Directive::Result pre_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Policy const& policy)
{
    switch (directive.kind()) {
    case Directive::Kind::ChildSrc:
        return child_src_pre_request_check(directive, request, policy);
    case Directive::Kind::ConnectSrc:
        return connect_src_pre_request_check(directive, request, policy);
    case Directive::Kind::DefaultSrc:
        return default_src_pre_request_check(directive, request, policy);
    case Directive::Kind::FontSrc:
        return font_src_pre_request_check(directive, request, policy);
    case Directive::Kind::FrameSrc:
        return frame_src_pre_request_check(directive, request, policy);
    case Directive::Kind::ImgSrc:
        return img_src_pre_request_check(directive, request, policy);
    case Directive::Kind::ManifestSrc:
        return manifest_src_pre_request_check(directive, request, policy);
    case Directive::Kind::MediaSrc:
        return media_src_pre_request_check(directive, request, policy);
    case Directive::Kind::ObjectSrc:
        return object_src_pre_request_check(directive, request, policy);
    case Directive::Kind::ScriptSrc:
        return script_src_pre_request_check(directive, request, policy);
    case Directive::Kind::ScriptSrcElem:
        return script_src_elem_pre_request_check(directive, request, policy);
    case Directive::Kind::StyleSrc:
        return style_src_pre_request_check(directive, request, policy);
    case Directive::Kind::StyleSrcElem:
        return style_src_elem_pre_request_check(directive, request, policy);
    case Directive::Kind::WorkerSrc:
        return worker_src_pre_request_check(directive, request, policy);
    default:
        return Directive::Result::Allowed;
    }
}

Directive::Result post_request_check(Directive const& directive, Fetch::Infrastructure::Request const& request, Fetch::Infrastructure::Response const& response, Policy const& policy)
{
    switch (directive.kind()) {
    case Directive::Kind::ChildSrc:
        return child_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::ConnectSrc:
        return connect_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::DefaultSrc:
        return default_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::FontSrc:
        return font_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::FrameSrc:
        return frame_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::ImgSrc:
        return img_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::ManifestSrc:
        return manifest_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::MediaSrc:
        return media_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::ObjectSrc:
        return object_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::ScriptSrc:
        return script_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::ScriptSrcElem:
        return script_src_elem_post_request_check(directive, request, response, policy);
    case Directive::Kind::StyleSrc:
        return style_src_post_request_check(directive, request, response, policy);
    case Directive::Kind::StyleSrcElem:
        return style_src_elem_post_request_check(directive, request, response, policy);
    case Directive::Kind::WorkerSrc:
        return worker_src_post_request_check(directive, request, response, policy);
    default:
        return Directive::Result::Allowed;
    }
}

}
