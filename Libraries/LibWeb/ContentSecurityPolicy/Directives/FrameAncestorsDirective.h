/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-frame-ancestors
class FrameAncestorsDirective final : public Directive {
    GC_CELL(FrameAncestorsDirective, Directive)
    GC_DECLARE_ALLOCATOR(FrameAncestorsDirective);

public:
    virtual ~FrameAncestorsDirective() = default;

    virtual Result navigation_response_check(GC::Ref<Fetch::Infrastructure::Request const>, NavigationType, GC::Ref<Fetch::Infrastructure::Response const>, GC::Ref<HTML::Navigable const>, CheckType, GC::Ref<Policy const>) const override;

private:
    FrameAncestorsDirective(String name, Vector<String> value);
};

}
