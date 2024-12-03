/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-style-src-attr
class StyleSourceAttributeDirective final : public Directive {
    GC_CELL(StyleSourceAttributeDirective, Directive)
    GC_DECLARE_ALLOCATOR(StyleSourceAttributeDirective);

public:
    virtual ~StyleSourceAttributeDirective() = default;

    virtual Result inline_check(GC::Heap&, GC::Ptr<DOM::Element const>, InlineType, GC::Ref<Policy const>, String const&) const override;

private:
    StyleSourceAttributeDirective(String name, Vector<String> value);
};

}
