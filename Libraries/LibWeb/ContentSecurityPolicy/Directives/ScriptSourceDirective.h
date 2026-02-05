/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-script-src
class ScriptSourceDirective final : public Directive {
    GC_CELL(ScriptSourceDirective, Directive)
    GC_DECLARE_ALLOCATOR(ScriptSourceDirective);

public:
    virtual ~ScriptSourceDirective() = default;

    virtual Result pre_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Policy const>) const override;
    virtual Result post_request_check(GC::Heap&, GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Fetch::Infrastructure::Response const>, GC::Ref<Policy const>) const override;
    virtual Result inline_check(GC::Heap&, GC::Ptr<DOM::Element const>, InlineType, GC::Ref<Policy const>, String const&) const override;

private:
    ScriptSourceDirective(String name, Vector<String> value);
};

}
