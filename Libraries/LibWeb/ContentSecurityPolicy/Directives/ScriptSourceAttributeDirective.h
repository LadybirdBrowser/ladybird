/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-script-src-attr
class ScriptSourceAttributeDirective final : public Directive {
    GC_CELL(ScriptSourceAttributeDirective, Directive)
    GC_DECLARE_ALLOCATOR(ScriptSourceAttributeDirective);

public:
    virtual ~ScriptSourceAttributeDirective() = default;

    virtual Result inline_check(GC::Heap&, GC::Ptr<DOM::Element const>, InlineType, GC::Ref<Policy const>, String const&) const override;

private:
    ScriptSourceAttributeDirective(String name, Vector<String> value);
};

}
