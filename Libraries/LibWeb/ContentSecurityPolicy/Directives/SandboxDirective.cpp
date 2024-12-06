/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/SandboxDirective.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(SandboxDirective);

SandboxDirective::SandboxDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

// https://w3c.github.io/webappsec-csp/#sandbox-init
Directive::Result SandboxDirective::initialization(Variant<GC::Ref<DOM::Document const>, GC::Ref<HTML::WorkerGlobalScope const>> context, GC::Ref<Policy const> policy) const
{
    // 1. If policy’s disposition is not "enforce", or context is not a WorkerGlobalScope, then abort this algorithm.
    // FIXME: File spec issue that this step doesn't specify the return value. It must be allowed, because Document
    //        asserts that the result of this algorithm is Allowed.
    if (policy->disposition() != Policy::Disposition::Enforce || !context.has<GC::Ref<HTML::WorkerGlobalScope const>>())
        return Result::Allowed;

    // 2. Let sandboxing flag set be a new sandboxing flag set.
    // 3. Parse a sandboxing directive using this directive’s value as the input, and sandboxing flag set as the output.
    // FIXME: File spec issue that "parse a sandboxing directive" does not accept a set of tokens.
    auto sandboxing_flag_set = HTML::parse_a_sandboxing_directive(value());

    // 4. If sandboxing flag set contains either the sandboxed scripts browsing context flag or the sandboxed origin
    //    browsing context flag flags, return "Blocked".
    // Spec Note: This will need to change if we allow Workers to be sandboxed into unique origins, which seems like a
    //            pretty reasonable thing to do.
    if (has_flag(sandboxing_flag_set, HTML::SandboxingFlagSet::SandboxedScripts) || has_flag(sandboxing_flag_set, HTML::SandboxingFlagSet::SandboxedOrigin))
        return Result::Blocked;

    // 5. Return "Allowed".
    return Result::Allowed;
}

}
