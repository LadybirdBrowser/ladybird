/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWebCommon/ContentSecurityPolicy/SerializedPolicy.h>
#include <LibWebCommon/HTML/SandboxingFlagSet.h>

namespace Web::ContentSecurityPolicy {

PolicyList::PolicyList(Vector<SerializedPolicy> const& serialized_policies)
{
    for (auto const& serialized_policy : serialized_policies)
        m_policies.append(Policy::create_from_serialized_policy(serialized_policy));
}

// https://w3c.github.io/webappsec-csp/#contains-a-header-delivered-content-security-policy
bool PolicyList::contains_header_delivered_policy() const
{
    // A CSP list contains a header-delivered Content Security Policy if it contains a policy whose source is "header".
    auto header_delivered_entry = m_policies.find_if([](auto const& policy) {
        return policy->source() == Policy::Source::Header;
    });

    return !header_delivered_entry.is_end();
}

// https://html.spec.whatwg.org/multipage/browsers.html#csp-derived-sandboxing-flags
HTML::SandboxingFlagSet PolicyList::csp_derived_sandboxing_flags() const
{
    // 1. Let directives be an empty ordered set.
    // NOTE: Since the algorithm only uses the last entry, we instead use a pointer to the last entry.
    Directives::Directive const* sandbox_directive = nullptr;

    // 2. For each policy in cspList:
    for (auto const& policy : m_policies) {
        // 1. If policy's disposition is not "enforce", then continue.
        if (policy->disposition() != Policy::Disposition::Enforce)
            continue;

        // 2. If policy's directive set contains a directive whose name is "sandbox", then append that directive to
        //   directives.
        auto maybe_sandbox_directive = policy->directives().find_if([](auto const& directive) {
            return directive.kind() == Directives::Directive::Kind::Sandbox;
        });

        if (!maybe_sandbox_directive.is_end())
            sandbox_directive = &*maybe_sandbox_directive;
    }

    // 3. If directives is empty, then return an empty sandboxing flag set.
    if (!sandbox_directive)
        return HTML::SandboxingFlagSet {};

    // 4. Let directive be directives[directives's size − 1].
    // NOTE: Already done.

    // 5. Return the result of parsing the sandboxing directive directive.
    return HTML::parse_a_sandboxing_directive(sandbox_directive->value());
}

// https://w3c.github.io/webappsec-csp/#enforced
void PolicyList::enforce_policy(NonnullRefPtr<Policy const> policy)
{
    // A policy is enforced or monitored for a global object by inserting it into the global object’s CSP list.
    m_policies.append(move(policy));
}

Vector<SerializedPolicy> PolicyList::serialize() const
{
    Vector<SerializedPolicy> serialized_policies;

    for (auto const& policy : m_policies)
        serialized_policies.append(policy->serialize());

    return serialized_policies;
}

}
